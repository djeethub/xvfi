#pragma once

#include <mat.h>

#include "ffmpeg.hpp"

namespace ff {

const AVPixelFormat out_fmt = AV_PIX_FMT_YUV420P10LE;

class VideoEncoder {
private:
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* audio_codec_ctx = nullptr;
    int audio_stream_index = -1;
    AVCodecContext* video_codec_ctx = nullptr;
    AVStream *video_stream;
    int video_stream_index = -1;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    std::thread thread;
    SwsContext* sws_ctx = nullptr;
    AVBufferPool *converted_pool = nullptr;
    int n_frame;

public:
    moodycamel::ReaderWriterQueue<ncnn::Mat> video_frame_queue;
    std::mutex mutex;
    std::condition_variable cv;
    Status status = Play;
    std::atomic<bool> is_eof;
    std::vector<std::pair<std::string, std::string>> args;

    ~VideoEncoder() {
        close();
        av_packet_free(&packet);
        av_frame_free(&frame);
    }

    bool open(const std::string &filename, int width, int height, int fps, AVDictionary *metadata)
    {
        close();

        if (avformat_alloc_output_context2(&format_ctx, nullptr, nullptr, filename.c_str()) < 0)
        {
            std::cerr << "Could not open video file.\n";
            return false;
        }

//        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AV1);
        const AVCodec* codec = avcodec_find_encoder_by_name("libsvtav1");
        if (!codec) {
            std::cerr << "Video encoder not found.\n";
            return false;
        }
        video_stream = avformat_new_stream(format_ctx, nullptr);
        video_codec_ctx = avcodec_alloc_context3(codec);

        video_codec_ctx->width = width;
        video_codec_ctx->height = height;
        video_codec_ctx->time_base = {1, fps};
        video_codec_ctx->framerate = {fps, 1};
        video_codec_ctx->pix_fmt = out_fmt;
        video_codec_ctx->thread_count = 0;
        video_codec_ctx->thread_type = FF_THREAD_FRAME;

        if (format_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
            format_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        AVDictionary* param_opts = nullptr;
        for (auto arg : args) {
            av_dict_set(&param_opts, arg.first.c_str(), arg.second.c_str(), 0);
        }
        avcodec_open2(video_codec_ctx, codec, &param_opts);
        av_dict_free(&param_opts);

        avcodec_parameters_from_context(video_stream->codecpar, video_codec_ctx);
        video_stream->time_base = video_codec_ctx->time_base;

        int err;
        // Open destination file
        if (!(format_ctx->oformat->flags & AVFMT_NOFILE)) {
            err = avio_open(&format_ctx->pb, filename.c_str(), AVIO_FLAG_WRITE);
            if (err < 0) {
                std::cerr << "avio_open failed: " << VideoFile::av_err2string(err) << std::endl;
                return false;
            }
        }
        if (metadata)
            av_dict_copy(&format_ctx->metadata, metadata, 0);
        err = avformat_write_header(format_ctx, nullptr);
        if (err < 0) {
            std::cerr << "avformat_write_header failed: " << VideoFile::av_err2string(err) << std::endl;
            return false;
        }

        if (!packet) packet = av_packet_alloc();
        is_eof.store(false, std::memory_order_relaxed);
        n_frame = 0;
        sws_ctx = sws_getContext(
            width, height, rgb_fmt,
            width, height, video_codec_ctx->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        return true;
    }

    void close() {
        avcodec_free_context(&audio_codec_ctx);
        avcodec_free_context(&video_codec_ctx);
        if (format_ctx) {
            if (!(format_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&format_ctx->pb);
            avformat_free_context(format_ctx);
            format_ctx = nullptr;
        }

        sws_free_context(&sws_ctx);
        av_buffer_pool_uninit(&converted_pool);
        frame_recycle(frame);
        frame = nullptr;
        audio_stream_index = -1;
        video_stream_index = -1;
    }

    AVFrame *alloc_converted_frame()
    {
        AVFrame *frame = frame_alloc();
        frame->format = out_fmt;
        frame->width  = video_codec_ctx->width;
        frame->height = video_codec_ctx->height;

        if (!converted_pool) {
            int buffer_size = av_image_get_buffer_size(static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, 32);
            converted_pool = av_buffer_pool_init(buffer_size, NULL);
        }

        // 2. Instead of av_frame_get_buffer, grab a buffer from your pool
        frame->buf[0] = av_buffer_pool_get(converted_pool);

        // 3. Link the frame's data pointers to the pool buffer
        av_image_fill_arrays(frame->data, frame->linesize,
                            frame->buf[0]->data, static_cast<AVPixelFormat>(frame->format),
                            frame->width, frame->height, 32);

        return frame;
    }

    int encode() {
        if (is_eof.load(std::memory_order_relaxed))
            return LARGE_INTERVAL;

        ncnn::Mat mat;
        while (video_frame_queue.try_dequeue(mat)) {
            if (mat.empty()) {
                // Flush encoder
                avcodec_send_frame(video_codec_ctx, nullptr);
                while (avcodec_receive_packet(video_codec_ctx, packet) == 0) {
                    av_packet_rescale_ts(packet, video_codec_ctx->time_base, video_stream->time_base);
                    av_interleaved_write_frame(format_ctx, packet);
                    av_packet_unref(packet);
                }

                av_write_trailer(format_ctx);
                is_eof.store(true, std::memory_order_relaxed);
                std::cout << "encoder done.\n";
                return LARGE_INTERVAL;
            }

            // Allocate YUV Frame container for the encoder
            if (!frame) frame = alloc_converted_frame();
            av_frame_make_writable(frame);

            int linesize[4] = { (int)(mat.w * mat.c * mat.elemsize), 0, 0, 0 };
            const uint8_t* src_data[4] = { (const uint8_t*)mat.data, nullptr, nullptr, nullptr };
            sws_scale(sws_ctx, src_data, linesize, 0, mat.h, frame->data, frame->linesize);
            frame->pts = n_frame++;

            // Push frame to encoder
            avcodec_send_frame(video_codec_ctx, frame);
            while (avcodec_receive_packet(video_codec_ctx, packet) == 0) {
                av_packet_rescale_ts(packet, video_codec_ctx->time_base, video_stream->time_base);
                packet->stream_index = video_stream->index;
                av_interleaved_write_frame(format_ctx, packet);
                av_packet_unref(packet);
            }
        }

        return 0.04;
    }

    static void thread_worker(VideoEncoder *video)
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(video->mutex);
            if (video->status == Quit)
                break;
            video->status = Play;
            double interval = video->encode();
            video->cv.wait_for(lock, std::chrono::microseconds(static_cast<int64_t>(interval * 1000000)), [video]{ return video->status != Play; });
        }
    }

    void start_thread() {
        if (!thread.joinable()) {
            thread = std::thread(thread_worker, this);
#ifdef __linux__
            pthread_setname_np(thread.native_handle(), "encoder");
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
};
}