#pragma once

#include "ffmpeg.hpp"
#include "encode.hpp"
#include "realcugan.h"
#include "rife.h"

class AppState {
public:
    ff::VideoFile video;
    ff::VideoEncoder encoder;
    const int scale = 2;
#ifdef __linux__
    const char *realcugan_param_path = "/mnt/d/Linux/Project/vendor/realcugan-ncnn-vulkan/models/models-pro/up2x-conservative.param";
    const char *realcugan_model_path = "/mnt/d/Linux/Project/vendor/realcugan-ncnn-vulkan/models/models-pro/up2x-conservative.bin";
    const char *rife_model_path = "/mnt/d/Linux/Project/vendor/rife-ncnn-vulkan/models/rife-v4.26";
#else
    const WCHAR *realcugan_param_path = L"D:/Linux/Project/vendor/realcugan-ncnn-vulkan/models/models-pro/up2x-conservative.param";
    const WCHAR *realcugan_model_path = L"D:/Linux/Project/vendor/realcugan-ncnn-vulkan/models/models-pro/up2x-conservative.bin";
    const WCHAR *rife_model_path = L"D:/Linux/Project/vendor/rife-ncnn-vulkan/models/rife-v4.26";
#endif
    int x_frames = 1;
    const int target_fps = 60;

    AppState() {
        av_log_set_level(AV_LOG_WARNING);
    }
    ~AppState() {
        shutdown();
    }

    bool open(const std::string& in_path, const std::string& out_path) {
        {
            std::scoped_lock lock(video.mutex, encoder.mutex);
            reset_runtime_state();

            if (!video.open(in_path))
                return false;

            if (video.find_video_stream()) {
                if (video.open_video_decoder()) {
                    std::cout << "in_pix_fmt: " << av_get_pix_fmt_name(video.get_video_ctx()->pix_fmt) << std::endl;
                }
            }
            auto video_ctx = video.get_video_ctx();
            if (!video_ctx)
                return false;
            auto fps = video.get_fps();
            if (fps == 0) {
                std::cout << "fps not available.";
                return false;
            }
            while (fps * x_frames < target_fps) {
                x_frames++;
            }
            fps *= x_frames;

            auto format_ctx = video.get_format_ctx();
            if (!encoder.open(out_path, video_ctx->width * scale, video_ctx->height * scale, fps, format_ctx->metadata))
                return false;
            video.seek_time = video.get_start_time();
            video.is_seeking = true;
            video.status = ff::Reset;
            encoder.status = ff::Reset;
        }
        video.cv.notify_one();
        encoder.cv.notify_one();
        video.start_thread();
        encoder.start_thread();
        return true;
    }

    void clear_frame_buffers() {
        AVFrame *frame;
        while (video.video_frame_queue.try_dequeue(frame))
            ff::frame_recycle(frame);
        while (video.audio_frame_queue.try_dequeue(frame))
            ff::frame_recycle(frame);
        ncnn::Mat *mat;
        while (encoder.video_frame_queue.try_dequeue(mat))
            delete mat;
        ff::AVSubtitle_ *sub;
        while (video.sub_queue.try_dequeue(sub))
            ff::subtitle_recycle(sub);
    }

    void reset_runtime_state() {
        video.close();
        encoder.close();
        clear_frame_buffers();
    }

    bool shutdown() {
        video.stop_thead();
        encoder.stop_thead();
        reset_runtime_state();
        return true;
    }

    RealCUGAN *load_realcugan() {
        auto gpuid = ncnn::get_default_gpu_index();

        int tilesize = 0;
        int prepadding = 0;

        uint32_t heap_budget = ncnn::get_gpu_device(gpuid)->get_heap_budget();
        if (scale == 2)
        {
            if (heap_budget > 1300)
                tilesize = 400;
            else if (heap_budget > 800)
                tilesize = 300;
            else if (heap_budget > 400)
                tilesize = 200;
            else if (heap_budget > 200)
                tilesize = 100;
            else
                tilesize = 32;
            prepadding = 18;
        }
        if (scale == 3)
        {
            if (heap_budget > 3300)
                tilesize = 400;
            else if (heap_budget > 1900)
                tilesize = 300;
            else if (heap_budget > 950)
                tilesize = 200;
            else if (heap_budget > 320)
                tilesize = 100;
            else
                tilesize = 32;
            prepadding = 14;
        }
        if (scale == 4)
        {
            if (heap_budget > 1690)
                tilesize = 400;
            else if (heap_budget > 980)
                tilesize = 300;
            else if (heap_budget > 530)
                tilesize = 200;
            else if (heap_budget > 240)
                tilesize = 100;
            else
                tilesize = 32;
            prepadding = 19;
        }

        auto data = new RealCUGAN(gpuid, 0, 1);
        data->load(realcugan_param_path, realcugan_model_path);

        data->noise = -1;
        data->scale = scale;
        data->tilesize = tilesize;
        data->prepadding = prepadding;
        data->syncgap = 3;
        return data;
    }

    RIFE *load_rife() {
        auto gpuid = ncnn::get_default_gpu_index();

        int padding = 32;
/*        if (model.find(PATHSTR("rife-v4.25")) != path_t::npos)
        {
            padding = 64;
        }
        if (model.find(PATHSTR("rife-v4.25-lite")) != path_t::npos)
        {
            padding = 128;
        }
        if (model.find(PATHSTR("rife-v4.26")) != path_t::npos)
        {
            padding = 64;
        }*/
        padding = 64;   // rife-v4.26

        auto data = new RIFE(gpuid, 0, 0, 0, 1, false, true, padding);
        data->load(rife_model_path);
        return data;
    }

    bool process() {
        AVFrame *frame = nullptr;

        ncnn::create_gpu_instance();
        const RealCUGAN *scaler = load_realcugan();
        const RIFE *vfi = load_rife();

        ncnn::Mat prev_image;
        while (true) {
            while (video.video_frame_queue.try_dequeue(frame))
            {
                auto inimage = ncnn::Mat(frame->width, frame->height, (void*)frame->data[0], 3, 3);
                auto outimage = ncnn::Mat(frame->width * scaler->scale, frame->height * scaler->scale, (size_t)inimage.elemsize, (int)inimage.elemsize);
                scaler->process(inimage, outimage);
                if (!prev_image.empty()) {
                    for (auto i = 1; i < x_frames; i++) {
                        auto new_image = ncnn::Mat(outimage.w, outimage.h, (size_t)outimage.elemsize, (int)outimage.elemsize);
                        vfi->process(prev_image, outimage, (float) i / x_frames, new_image);
                        encoder.video_frame_queue.enqueue(new_image);
                    }
                }
                encoder.video_frame_queue.enqueue(outimage);
                prev_image = outimage;

                ff::frame_recycle(frame);
            }
            if (video.is_eof.load(std::memory_order_relaxed)) {
                encoder.video_frame_queue.enqueue(ncnn::Mat());
                std::cout << "xvfi done.\n";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        prev_image.release();

        delete scaler;
        delete vfi;

        while (!encoder.is_eof.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }

        ncnn::destroy_gpu_instance();

        return true;
    }
};
