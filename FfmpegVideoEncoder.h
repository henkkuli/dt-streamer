#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "FfmpegException.h"
#include "FfmpegMuxer.h"

class FfmpegVideoEncoder {
public:
    static std::unique_ptr<FfmpegVideoEncoder> CreateEncoder(std::shared_ptr<boost::asio::io_service> io_service,
                                                             const std::string& codec_name,
                                                             std::shared_ptr<FfmpegMuxer> muxer,
                                                             int width, int height, int64_t bit_rate = 10e6) {
        std::unique_ptr<FfmpegVideoEncoder> encoder(new FfmpegVideoEncoder(io_service, codec_name, muxer,
                                                                           width, height, bit_rate));
        return encoder;
    }
    
    ~FfmpegVideoEncoder() {
        avcodec_close(context);
        avcodec_free_context(&context);
        LOG(ERROR) << "FfmpegVideoEncoder destructed";
        exit(3);
    }

    AVFrame* GetNextFrame() {
        std::scoped_lock lock(frame_mutex);
        // TODO: Synchronization
        THROW_ON_AV_ERROR(av_frame_make_writable(background_frame.get()));
        return background_frame.get();
    }

    void SwapFrames() {
        std::scoped_lock lock(frame_mutex);
        // TODO: Synchronization
        std::swap(background_frame, foreground_frame);
    }

    void WriteFrame() {
        if (writing_frame.exchange(true)) return;

        io_service->post(std::bind(&FfmpegVideoEncoder::WriteFrameInternal, this));
    }

    void WriteFrameInternal() {
        std::scoped_lock lock(frame_mutex);
        THROW_ON_AV_ERROR(avcodec_send_frame(context, foreground_frame.get()));

        while (1) {
            int error_number = avcodec_receive_packet(context, packet.get());
            if (error_number == AVERROR(EAGAIN) || error_number == AVERROR_EOF) break;
            THROW_ON_AV_ERROR(error_number);

            // Timebase should be correct so no need for rescaling
            // av_packet_rescale_ts(packet, format_context->streams[packet->stream_index]->time_base, output_stream->time_base);
            packet->stream_index = stream->index;

            muxer->WritePacket(packet.get());
        }

        writing_frame = false;
    }

    int GetWidth() {
        return context->width;
    }

    int GetHeight() {
        return context->height;
    }

private:
    FfmpegVideoEncoder(std::shared_ptr<boost::asio::io_service> _io_service, const std::string& codec_name,
                        std::shared_ptr<FfmpegMuxer> _muxer, int width, int height, int64_t bit_rate) :
                        io_service(_io_service), muxer(std::move(_muxer)) {
        codec = avcodec_find_encoder_by_name(codec_name.c_str());
        if (!codec) THROW_FFMPEG("Codec " + codec_name + " not found");

        context = avcodec_alloc_context3(codec);
        if (!context) THROW_FFMPEG("Can't create codec context");

        context->bit_rate = bit_rate;
        context->width = width;
        context->height = height;
        context->time_base = muxer->GetTimeBase();
        context->framerate = muxer->GetFrameRate();
        context->gop_size = 10;
        context->max_b_frames = 1;
        context->pix_fmt = AV_PIX_FMT_YUV420P;
        context->thread_count = 1;

        THROW_ON_AV_ERROR(avcodec_open2(context, codec, nullptr));

        foreground_frame = AllocateFrame();
        background_frame = AllocateFrame();

        packet = std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>(av_packet_alloc(), [](AVPacket* av_packet) {
            av_packet_free(&av_packet);
        });
        if (!packet) THROW_FFMPEG("Failed to allocate packet");

        stream = muxer->NewStream();
        THROW_ON_AV_ERROR(avcodec_parameters_from_context(stream->codecpar, context));
    }
    
    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> AllocateFrame() {
        AVFrame* frame = av_frame_alloc();
        if (!frame) THROW_FFMPEG("Failed to allocate frame");

        frame->format = context->pix_fmt;
        frame->width = context->width;
        frame->height = context->height;

        THROW_ON_AV_ERROR(av_frame_get_buffer(frame, 32));
        return std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>(frame, [](AVFrame* av_frame) {
            av_frame_free(&av_frame);
        });
    }

    std::shared_ptr<boost::asio::io_service> io_service;
    std::shared_ptr<FfmpegMuxer> muxer;
    AVCodec* codec;
    AVCodecContext* context;
    AVStream* stream;
    std::mutex frame_mutex;
    std::atomic<bool> writing_frame;
    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> foreground_frame;
    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> background_frame;
    std::unique_ptr<AVPacket, std::function<void(AVPacket*)>> packet;
};
