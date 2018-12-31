#pragma once

#include <memory>
#include <string>
#include <thread>
extern "C" {
#include <libavformat/avformat.h>
}

#include "FfmpegException.h"
#include "FfmpegInput.h"
#include "FfmpegVideoDecoder.h"

class FfmpegDemuxer {
public:
    FfmpegDemuxer(AVInputFormat* input_format, std::unique_ptr<FfmpegInput> _input) : input(std::move(_input)) {
        format_context = avformat_alloc_context();
        if (!format_context) THROW_FFMPEG("Failed to allocate format context");
        format_context->pb = input->GetAvioContext();

        THROW_ON_AV_ERROR(avformat_open_input(&format_context, nullptr, input_format, nullptr));
    }

    FfmpegVideoDecoder* FindVideoStream() {
        if (!format_info_decoded) {
            int error_number = avformat_find_stream_info(format_context, nullptr);
            if (error_number < 0) return nullptr;       // TODO: Check for error message types
        }
        for (unsigned int i = 0; i < format_context->nb_streams; i++) {
            AVStream* stream = format_context->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                return video_decoder = new FfmpegVideoDecoder(stream);
            }
        }

        return nullptr;
    }

    void DemuxNextFrame() {
        AVPacket* packet = av_packet_alloc();
        if (!packet) THROW_FFMPEG("Failed to allocate packet");
        int error_number = av_read_frame(format_context, packet);
        if (error_number == AVERROR(EAGAIN) || error_number == AVERROR_EOF) {
            av_packet_free(&packet);
            return;
        }
        THROW_ON_AV_ERROR(error_number);

        if (packet->size && packet->stream_index == video_decoder->GetStreamIndex()) {
            video_decoder->SendPacket(packet);
        } else {
            av_packet_unref(packet);
        }
    }

protected:
    AVFormatContext* format_context;
    std::unique_ptr<FfmpegInput> input;
    FfmpegVideoDecoder* video_decoder;
    bool format_info_decoded = false;

    FfmpegDemuxer() {}
};

class FfmpegX11grabDemuxer : public FfmpegDemuxer {
public:
    FfmpegX11grabDemuxer(const std::string& filename, int width, int height) {
        format_context = nullptr;

        AVDictionary* options = nullptr;
        std::string video_size = std::to_string(width) + "x" + std::to_string(height);
        av_dict_set(&options, "video_size", video_size.c_str(), 0);
        av_dict_set_int(&options, "framerate", 60, 0);

        auto input_format = av_find_input_format("x11grab");
        THROW_ON_AV_ERROR(avformat_open_input(&format_context, filename.c_str(), input_format, &options));
    }
};
