#pragma once

#include <string>
#include <unordered_set>
#include <format>
#include <vector>
#include <condition_variable>
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/dict.h>
}

#include "concurrentqueue.h"
#include "readerwriterqueue.h"

namespace ff {

enum Status {
    Play = 0,
    Reset,
    Quit,
};

const auto LARGE_INTERVAL = 777777.7;
const double PRELOAD_TIME  = 0.2;
const AVPixelFormat rgb_fmt = AV_PIX_FMT_RGB24;

struct Subtitle {
    std::string text;
    double pts;
    double duration;
};

struct ChapterData {
    std::string title;
    double start_time; // in seconds
    double end_time;   // in seconds
};

struct AudioData {
    int idx;
    std::string lang;
    std::string title;
    AVCodecID codec_id;
};

struct AudioBuffer {
    uint8_t *buf = nullptr;
    int size = 0;
    int data_size = 0;

    AudioBuffer() = default;
    ~AudioBuffer() {
        av_freep(&buf);
    }
    void init(int n)
    {
        if (size < n) {
            av_freep(&buf);
            av_samples_alloc(&buf, nullptr, 2, n, AV_SAMPLE_FMT_S16, 0);
            size = n;
        }
    }
};

class FramePool {
protected:
    moodycamel::ConcurrentQueue<AVFrame *> recycle_queue;

public:
    ~FramePool() {
        AVFrame *frame;
        while (recycle_queue.try_dequeue(frame)) {
            av_frame_free(&frame);
        }
    }

    void recycle(AVFrame *frame) {
        if (!frame)
            return;
        av_frame_unref(frame);
        recycle_queue.enqueue(frame);
    }

    AVFrame *alloc() {
        AVFrame *frame;
        if (recycle_queue.try_dequeue(frame))
            return frame;
        return av_frame_alloc();
    }
};

struct AVSubtitle_ : public AVSubtitle {
    double frame_time;
    double duration;
};

class SubtitlePool {
protected:
    moodycamel::ConcurrentQueue<AVSubtitle_ *> recycle_queue;

public:
    ~SubtitlePool() {
        AVSubtitle_ *frame;
        while (recycle_queue.try_dequeue(frame)) {
            delete frame;
        }
    }

    void recycle(AVSubtitle_ *frame) {
        if (!frame)
            return;
        avsubtitle_free(frame);
        recycle_queue.enqueue(frame);
    }

    AVSubtitle_ *alloc() {
        AVSubtitle_ *frame;
        if (recycle_queue.try_dequeue(frame))
            return frame;
        return new AVSubtitle_;
    }
};

FramePool frame_pool;
SubtitlePool sub_pool;

inline AVFrame *frame_alloc() {
    return frame_pool.alloc();
}

inline void frame_recycle(AVFrame *frame) {
    frame_pool.recycle(frame);
}

inline AVSubtitle_ *subtitle_alloc() {
    return sub_pool.alloc();
}

inline void subtitle_recycle(AVSubtitle_ *frame) {
    sub_pool.recycle(frame);
}

class VideoFile *_video;

class VideoFile {
private:
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* audio_codec_ctx = nullptr;
    int audio_stream_index = -1;
    AVCodecContext* video_codec_ctx = nullptr;
    int video_stream_index = -1;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    std::vector<AudioData> subtitle_list;
    std::vector<AudioData> audio_list;
    std::vector<std::string> sub_lang_pref = {"en", "eng", "ja", "jpn"};
    std::vector<std::string> audio_lang_pref = {"ja", "jpn", "en", "eng"};
    int subtitle_stream_idx = -1;
    AVCodecContext* subtitle_codec_ctx = nullptr;
    double start_time;
    double duration;
    double video_time_base = 0.0;
    double audio_time_base = 0.0;
    double subtitle_time_base = 0.0;
    std::thread thread;
    double last_video_time;
    double last_audio_time;
    SwsContext *sws_ctx = nullptr;
    AVBufferPool *converted_pool = nullptr;

public:
    VideoFile() {
        _video = this;
    }
    ~VideoFile() {
        close();
        av_packet_free(&packet);
        av_frame_free(&frame);
    }

    VideoFile(const VideoFile&) = delete;
    VideoFile& operator=(const VideoFile&) = delete;
    VideoFile(VideoFile&&) = delete;
    VideoFile& operator=(VideoFile&&) = delete;

    auto get_start_time() { return start_time; }
    auto get_duration() const { return duration; }
    auto get_video_time_base() const { return video_time_base; }
    auto get_audio_time_base() const { return audio_time_base; }
    auto get_subtitle_time_base() const { return subtitle_time_base; }
    auto get_subtitle_tracks() const { return subtitle_list; }
    auto get_audio_tracks() const { return audio_list; }
    auto get_subtitle_index() const { return subtitle_stream_idx; }
    auto get_audio_index() const { return audio_stream_index; }
    auto get_subtitle_ctx() const { return subtitle_codec_ctx; }
    auto get_video_ctx() const { return video_codec_ctx; }
    auto get_audio_ctx() const { return audio_codec_ctx; }
    auto get_format_ctx() const { return format_ctx; }
    auto is_audio() const { return audio_codec_ctx != nullptr; }
    auto is_video() const { return video_codec_ctx != nullptr; }

    moodycamel::ReaderWriterQueue<AVFrame *> audio_frame_queue;
    moodycamel::ReaderWriterQueue<AVFrame *> video_frame_queue;
    moodycamel::ReaderWriterQueue<AVSubtitle_ *> sub_queue;
    std::mutex mutex;
    std::condition_variable cv;
    Status status = Play;
    bool is_paused = false;
    bool is_seeking = false;
    double seek_time;
    std::atomic<double> shared_tick;
    std::atomic<bool> is_eof;

    bool open(const std::string &filename)
    {
        close();

        if (avformat_open_input(&format_ctx, filename.c_str(), nullptr, nullptr) < 0)
        {
            std::cerr << "Could not open video file.\n";
            return false;
        }
        if (avformat_find_stream_info(format_ctx, nullptr) < 0)
        {
            std::cerr << "Could not find stream information.\n";
            close();
            return false;
        }
        start_time = 0;//format_ctx->start_time == AV_NOPTS_VALUE ? 0.0 : static_cast<double>(format_ctx->start_time) * AV_TIME_BASE;
        duration = static_cast<double>(format_ctx->duration) / AV_TIME_BASE;
        if (!packet) packet = av_packet_alloc();
        if (!frame) frame = frame_pool.alloc();
        is_eof.store(false, std::memory_order_relaxed);
        last_audio_time = start_time;
        last_video_time = start_time;
        return true;
    }

    std::string get_stream_metadata(const AVStream* stream, const char *key) {
        if (!stream || !stream->metadata) {
            return "";
        }

        // Look up the key in the stream's metadata dictionary
        AVDictionaryEntry* entry = av_dict_get(stream->metadata, key, nullptr, 0);
        if (entry && entry->value) {
            return std::string(entry->value);
        }

        return "";
    }

    bool find_subtitle_stream()
    {
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            auto stream = format_ctx->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                auto subtitle = AudioData();
                subtitle.idx = i;
                subtitle.lang = get_stream_metadata(stream, "language");
                subtitle.title = get_stream_metadata(stream, "title");
                if (subtitle.title.empty())
                    subtitle.title = std::format("Track {}", i);
                subtitle.codec_id = stream->codecpar->codec_id;
                subtitle_list.push_back(subtitle);
            }
        }

        for (const auto& val : sub_lang_pref) {
            for (const auto& sub : subtitle_list) {
                if (sub.lang == val) {
                    subtitle_stream_idx = sub.idx;
                    return true;
                }
            }
        }

        for (const auto& val : sub_lang_pref) {
            for (const auto& data : subtitle_list) {
                if (is_substring(data.title, val)) {
                    subtitle_stream_idx = data.idx;
                    return true;
                }
            }
        }

        return false;
    }

    static bool is_substring(const std::string& str, const std::string& sub) {
        std::string lower_str = str;
        std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), ::tolower);
        return lower_str.find(sub) != std::string::npos;
    }

    bool find_audio_stream()
    {
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            auto stream = format_ctx->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                auto data = AudioData();
                data.idx = i;
                data.lang = get_stream_metadata(stream, "language");
                data.title = get_stream_metadata(stream, "title");
                if (data.title.empty())
                    data.title = std::format("Track {}", i);
                data.codec_id = stream->codecpar->codec_id;
                audio_list.push_back(data);
            }
        }

        for (const auto& val : audio_lang_pref) {
            for (const auto& data : audio_list) {
                if (data.lang == val) {
                    audio_stream_index = data.idx;
                    return true;
                }
            }
        }

        for (const auto& val : audio_lang_pref) {
            for (const auto& data : audio_list) {
                if (is_substring(data.title, val)) {
                    audio_stream_index = data.idx;
                    return true;
                }
            }
        }

        if (!audio_list.empty()) {
            audio_stream_index = audio_list.front().idx;
            return true;
        }
//        std::cerr << "Could not find an audio stream.\n";
        return false;
    }

    bool find_video_stream()
    {
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++)
        {
            if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                video_stream_index = i;
                return true;
            }
        }
//        std::cerr << "Could not find a video stream.\n";
        return false;
    }

    bool open_audio_decoder()
    {
        AVCodecParameters *codec_params = format_ctx->streams[audio_stream_index]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
        audio_codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(audio_codec_ctx, codec_params);
        avcodec_open2(audio_codec_ctx, codec, nullptr);
        audio_time_base = av_q2d(format_ctx->streams[audio_stream_index]->time_base);
        return true;
    }

    bool open_video_decoder(const AVStream *stream, bool hwdec) {
        auto codec_params = stream->codecpar;
        const AVCodec *codec = nullptr;
        if (codec_params->codec_id == AV_CODEC_ID_AV1 && hwdec)
            codec = avcodec_find_decoder_by_name("av1");
        else
            codec = avcodec_find_decoder(codec_params->codec_id);
        if (!codec)
            return false;
        video_codec_ctx = avcodec_alloc_context3(codec);
        if (hwdec) {
#ifdef __linux__
            AVBufferRef *hw_device_ctx = nullptr;
            if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0) == 0) {
                video_codec_ctx->hw_device_ctx = hw_device_ctx;
            }
#else
            AVBufferRef *hw_device_ctx = nullptr;
            if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0) == 0) {
                video_codec_ctx->hw_device_ctx = hw_device_ctx;
            }
#endif
        } else {
            video_codec_ctx->thread_count = 0;
            video_codec_ctx->thread_type = FF_THREAD_FRAME; // Or FF_THREAD_SLICE
        }
        avcodec_parameters_to_context(video_codec_ctx, codec_params);
        auto err = avcodec_open2(video_codec_ctx, codec, nullptr);
        if (err) {
            avcodec_free_context(&video_codec_ctx);
            return false;
        }
        video_time_base = av_q2d(format_ctx->streams[video_stream_index]->time_base);
        return true;
    }

    bool open_video_decoder(bool hwdec = true)
    {
        auto stream = format_ctx->streams[video_stream_index];
        AVCodecParameters *codec_params = stream->codecpar;

        switch (codec_params->format) {
            case AV_PIX_FMT_GRAY8:
                hwdec = false;
        }
        if (hwdec && open_video_decoder(stream, true))
            return true;
        return open_video_decoder(stream, false);
    }

    bool open_subtitle_decoder() {
        AVCodecParameters *codec_params = format_ctx->streams[subtitle_stream_idx]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
        subtitle_codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(subtitle_codec_ctx, codec_params);
        avcodec_open2(subtitle_codec_ctx, codec, nullptr);
        subtitle_time_base = av_q2d(format_ctx->streams[subtitle_stream_idx]->time_base);
        return true;
    }

    void close() {
        avcodec_free_context(&audio_codec_ctx);
        avcodec_free_context(&video_codec_ctx);
        avcodec_free_context(&subtitle_codec_ctx);
        avformat_close_input(&format_ctx);
        sws_free_context(&sws_ctx);
        av_buffer_pool_uninit(&converted_pool);
        audio_stream_index = -1;
        video_stream_index = -1;
        subtitle_stream_idx = -1;
        subtitle_list.clear();
        audio_list.clear();
        is_seeking = false;
    }

    void get_video_dimensions(int& width, int& height) const {
        if (video_codec_ctx) {
            width = video_codec_ctx->width;
            height = video_codec_ctx->height;
        } else {
            width = 0;
            height = 0;
        }
    }

    int64_t seek_internal(int64_t ts)
    {
        int seek_result = avformat_seek_file(
                format_ctx,
                -1,
                INT64_MIN, ts, ts,
                AVSEEK_FLAG_BACKWARD);
        if (seek_result >= 0) {
            if (audio_codec_ctx)
                avcodec_flush_buffers(audio_codec_ctx);
            if (video_codec_ctx)
                avcodec_flush_buffers(video_codec_ctx);
            if (subtitle_codec_ctx)
                avcodec_flush_buffers(subtitle_codec_ctx);
            is_eof.store(false, std::memory_order_relaxed);
        }
        return seek_result;
    }

    bool seek(double ts) {
        if (seek_internal(static_cast<int64_t>(ts * AV_TIME_BASE)) >= 0) {
            set_seeking(true);
            if (ts > get_start_time())
                set_skip(AVDISCARD_NONREF, AVDISCARD_ALL, AVDISCARD_ALL);
            last_video_time = ts;
            last_audio_time = ts;
            return true;
        } else
            return false;
    }

    void set_seeking(bool set) {
        if (set) {
            is_seeking = true;
//            clear_frame_buffers();
        } else {
            is_seeking = false;
            set_skip(AVDISCARD_DEFAULT, AVDISCARD_DEFAULT, AVDISCARD_DEFAULT);
        }
    }

    std::vector<ChapterData> read_chapters() {
        std::vector<ChapterData> chapter_list;

        // 1. Check if the file actually contains any chapters
        if (format_ctx->nb_chapters == 0) {
            return chapter_list;
        }

        double start_time = get_start_time();
        // 2. Iterate through the chapters array
        for (unsigned int i = 0; i < format_ctx->nb_chapters; i++) {
            AVChapter* chapter = format_ctx->chapters[i];
            ChapterData data;

            // 3. Convert timestamps from the chapter's unique timebase into seconds
            double timebase_factor = av_q2d(chapter->time_base);
            data.start_time = start_time;
            data.end_time   = chapter->end * timebase_factor;

            // 4. Extract the chapter title string from the metadata dictionary
            AVDictionaryEntry* title_tag = av_dict_get(chapter->metadata, "title", nullptr, 0);
            if (title_tag && title_tag->value) {
                data.title = title_tag->value;
            } else {
                // Fallback string if the chapter is unnamed (e.g., "Chapter 1")
                data.title = "Chapter " + std::to_string(i + 1);
            }

            chapter_list.push_back(data);
            start_time = data.end_time;
        }

        return chapter_list;
    }

    static std::string av_err2string(int errnum) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(errnum, errbuf, sizeof(errbuf));
        return std::string(errbuf);
    }

    void select_subtitle(int idx) {
        if (subtitle_stream_idx != idx) {
            avcodec_free_context(&subtitle_codec_ctx);
            subtitle_stream_idx = idx;
            if (idx >= 0)
                open_subtitle_decoder();
        }
    }

    void select_audio(int idx) {
        if (audio_stream_index != idx) {
            avcodec_free_context(&audio_codec_ctx);
            audio_stream_index = idx;
            if (idx >= 0)
                open_audio_decoder();
        }
    }

    void set_skip(AVDiscard frame, AVDiscard loop_filter, AVDiscard idct) {
        video_codec_ctx->skip_frame = frame;
        video_codec_ctx->skip_loop_filter = loop_filter;
        video_codec_ctx->skip_idct = idct;
    }

	bool setup_sws_context(AVFrame *frame) {
        if (sws_ctx)
            return true;
		sws_ctx = sws_getContext(
			frame->width, frame->height, (AVPixelFormat) frame->format,       // Source video specs
			frame->width, frame->height, rgb_fmt,        // Destination specs (GPU friendly)
			SWS_BILINEAR,                          // Fast filter (since size is identical)
			NULL, NULL, NULL
		);
		return sws_ctx != nullptr;
	}

    AVFrame *alloc_converted_frame()
    {
        AVFrame *frame = frame_alloc();
        frame->format = rgb_fmt;
        frame->width  = video_codec_ctx->width;
        frame->height = video_codec_ctx->height;

        if (!converted_pool) {
            int buffer_size = av_image_get_buffer_size(static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, 1);
            converted_pool = av_buffer_pool_init(buffer_size, NULL);
        }

        // 2. Instead of av_frame_get_buffer, grab a buffer from your pool
        frame->buf[0] = av_buffer_pool_get(converted_pool);

        // 3. Link the frame's data pointers to the pool buffer
        av_image_fill_arrays(frame->data, frame->linesize,
                            frame->buf[0]->data, static_cast<AVPixelFormat>(frame->format),
                            frame->width, frame->height, 1);

        return frame;
    }

    int read_next_frame(double play_time, bool preload = false) {
        int read_result = 0;
        double target_play_time = play_time + PRELOAD_TIME;

        while (true) {
            read_result = av_read_frame(format_ctx, packet);

            if (packet->stream_index == audio_stream_index)
            {
                last_audio_time = packet->pts * get_audio_time_base();
                if (last_audio_time >= play_time && avcodec_send_packet(audio_codec_ctx, packet) >= 0)
                {
                    while (avcodec_receive_frame(audio_codec_ctx, frame) >= 0)
                    {
                        if (frame->pts != AV_NOPTS_VALUE) {
                            if (is_seeking) {
                                set_seeking(false);
                            }
                            auto new_frame = frame_alloc();
                            av_frame_move_ref(new_frame, frame);
                            audio_frame_queue.enqueue(new_frame);
                        }
                        av_frame_unref(frame);
                    }
                }
            }
            else if (packet->stream_index == video_stream_index)
            {
                if (avcodec_send_packet(video_codec_ctx, packet) >= 0)
                {
                    while (avcodec_receive_frame(video_codec_ctx, frame) >= 0)
                    {
                        if (frame->pts != AV_NOPTS_VALUE) {
                            last_video_time = frame->pts * get_video_time_base();
                            if (last_video_time >= play_time) {
                                if (is_seeking) {
                                    set_seeking(false);
                                }
                                if (frame->hw_frames_ctx) {
                                    auto sw_frame = frame_alloc();
                                    auto err = av_hwframe_transfer_data(sw_frame, frame, 0);
                                    if (err != 0) std::cerr << "av_hwframe_transfer_data failed: " << av_err2string(err) << std::endl;
                                    auto new_frame = alloc_converted_frame();
                                    setup_sws_context(sw_frame);
                                    sws_scale(sws_ctx, sw_frame->data, sw_frame->linesize, 0, frame->height, new_frame->data, new_frame->linesize);
                                    av_frame_copy_props(new_frame, frame);
                                    video_frame_queue.enqueue(new_frame);
                                    frame_recycle(sw_frame);
                                } else {
                                    auto new_frame = alloc_converted_frame();
                                    setup_sws_context(frame);
                                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, new_frame->data, new_frame->linesize);
                                    av_frame_copy_props(new_frame, frame);
                                    video_frame_queue.enqueue(new_frame);
                                }
                            }
                        }
                        av_frame_unref(frame);
                    }
                }
            }
            else if (packet->stream_index == subtitle_stream_idx) {
                int got_subtitle = 0;
                auto sub = subtitle_alloc();
                // avcodec_decode_subtitle2 is old but still the standard way to handle subtitles in modern FFmpeg
                if (avcodec_decode_subtitle2(subtitle_codec_ctx, sub, &got_subtitle, packet) >= 0) {
                    if (got_subtitle) {
                        sub->frame_time = packet->pts * get_subtitle_time_base();
                        if (sub->end_display_time)
                            sub->duration = sub->end_display_time / 1000.0;
                        else
                            sub->duration = packet->duration * get_subtitle_time_base();
                        sub_queue.enqueue(sub);
                    } else
                        subtitle_recycle(sub);
                }
            }
            av_packet_unref(packet);

            if (!preload && last_video_time > play_time)
                break;
            if (read_result < 0) {
                is_eof.store(true, std::memory_order_relaxed);
                std::cout << "decoder done.\n";
                break;
            }
        }

        return read_result;
    }

    double time_next_frame() {
        if (is_paused && !is_seeking)
            return LARGE_INTERVAL;
        double play_time = get_start_time();
        auto rlt = read_next_frame(play_time, true);
//            SDL_Log("%i %i\n", audio_frame_queue.size_approx(), video_frame_queue.size_approx());
        if (rlt < 0) {
            return LARGE_INTERVAL;
        }
        return 0.1;
    }

    static void thread_worker(VideoFile *video)
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(video->mutex);
            if (video->status == Quit)
                break;
            video->status = Play;
            double interval = video->time_next_frame();
            video->cv.wait_for(lock, std::chrono::microseconds(static_cast<int64_t>(interval * 1000000)), [video]{ return video->status != Play; });
        }
    }

    void start_thread() {
        if (!thread.joinable()) {
            thread = std::thread(thread_worker, this);
#ifdef __linux__
            pthread_setname_np(thread.native_handle(), "media");
#endif
        }
    }

    void stop_thead() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            status = Quit;
        }
        cv.notify_one();
        if (thread.joinable())
            thread.join();
    }

    double get_fps() {
        auto fps = av_q2d(video_codec_ctx->framerate);
        if (fps == 0) {
            auto stream = format_ctx->streams[video_stream_index];
            fps = av_q2d(stream->avg_frame_rate);
            if (fps == 0) {
                fps = av_q2d(stream->r_frame_rate);
                if (fps == 0) {
                    fps = 1.0 / av_q2d(video_codec_ctx->time_base);
                }
            }
        }
        return fps;
    }
};
} // namespace ff