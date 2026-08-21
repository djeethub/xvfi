#pragma once

#include "ffmpeg.hpp"
#include "ff_encode.hpp"
#include "realcugan.h"

class AppState {
public:
    ff::VideoFile video;
    ff::VideoEncoder encoder;
    const int scale = 2;
    int tilesize = 0;

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
            auto fps = av_q2d(video_ctx->framerate);
            
            if (!encoder.open(out_path, video_ctx->width * scale, video_ctx->height * scale, fps))
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

    RealCUGAN *alloc_realcugan() {
        auto gpuid = ncnn::get_default_gpu_index();

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
        }

        auto data = new RealCUGAN(gpuid, 0, 2);
        auto paramfullpath = "/mnt/d/Linux/Project/vendor/realcugan-ncnn-vulkan/models/models-se/up2x-conservative.param";
        auto modelfullpath = "/mnt/d/Linux/Project/vendor/realcugan-ncnn-vulkan/models/models-se/up2x-conservative.bin";
        data->load(paramfullpath, modelfullpath);

        data->noise = -1;
        data->scale = scale;
        data->tilesize = tilesize;
        data->prepadding = 18;
        data->syncgap = 3;
        return data;
    }

    bool process() {
        AVFrame *frame = nullptr;

        ncnn::create_gpu_instance();
        const RealCUGAN* realcugan = alloc_realcugan();

        while (true) {
            while (video.video_frame_queue.try_dequeue(frame))
            {
                auto inimage = ncnn::Mat(frame->width, frame->height, (void*)frame->data, 3, 3);
                auto outimage = new ncnn::Mat(frame->width * realcugan->scale, frame->height * realcugan->scale, (size_t)inimage.elemsize, (int)inimage.elemsize);
                realcugan->process(inimage, *outimage);
                encoder.video_frame_queue.enqueue(outimage);
                
                ff::frame_recycle(frame);
            }
            if (video.is_eof.load(std::memory_order_acquire)) {
                encoder.video_frame_queue.enqueue(nullptr);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        while (!encoder.is_eof.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        delete realcugan;
        ncnn::destroy_gpu_instance();

        return true;
    }
};
