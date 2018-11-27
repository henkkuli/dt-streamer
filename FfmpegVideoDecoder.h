#pragma once

#include <functional>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "FfmpegException.h"

class FfmpegVideoDecoder {
public:

    FfmpegVideoDecoder(AVStream* _stream) : stream(_stream) {
        codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) THROW_FFMPEG("Codec " + std::to_string(stream->codecpar->codec_id) + " not found");
        
        // AVPacket* packet = av_packet_alloc();

        context = avcodec_alloc_context3(codec);
        if (!context) THROW_FFMPEG("Context not found");
        THROW_ON_AV_ERROR(avcodec_parameters_to_context(context, stream->codecpar));
        THROW_ON_AV_ERROR(avcodec_open2(context, codec, nullptr));

        frame = AllocateFrame();
    }

    void SendPacket(AVPacket* packet) {
        int ret = avcodec_send_packet(context, packet);
        if (ret < 0) THROW_FFMPEG("Failed to send packet");

        while (ret >= 0) {
            ret = avcodec_receive_frame(context, frame);
            // TODO: Correct error handling
            if (ret == AVERROR(EAGAIN)) break;

            if (on_frame) on_frame(frame);
        }

        av_packet_unref(packet);
    }
    
    void OnFrame(std::function<void(AVFrame*)> _on_frame) {
        on_frame = _on_frame;
    }

    int GetStreamIndex() {
        return stream->index;
    }

private:
    AVFrame* AllocateFrame() {
        AVFrame* tmpframe = av_frame_alloc();
        if (!tmpframe) THROW_FFMPEG("Failed to allocate frame");

        tmpframe->format = context->pix_fmt;
        tmpframe->width = context->width;
        tmpframe->height = context->height;

        THROW_ON_AV_ERROR(av_frame_get_buffer(tmpframe, 32));
        return tmpframe;
    }


    AVStream* stream;
    AVCodec* codec;
    AVCodecContext* context;
    AVFrame* frame;
    std::function<void(AVFrame*)> on_frame;
};
