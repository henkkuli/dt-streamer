#pragma once

#include <memory>
extern "C" {
#include <libavformat/avformat.h>
}

#include "FfmpegException.h"
#include "FfmpegOutput.h"

class FfmpegMuxer {
public:
    FfmpegMuxer(AVOutputFormat* output_format, std::unique_ptr<FfmpegOutput> _output, int _frame_rate) :
                output(std::move(_output)), time_base{1, _frame_rate}, frame_rate{_frame_rate, 1} {
        THROW_ON_AV_ERROR(avformat_alloc_output_context2(&format_context, output_format, nullptr, nullptr));
        format_context->pb = output->GetAvioContext();
    }

    ~FfmpegMuxer() {
        avformat_free_context(format_context);
    }

    AVRational GetTimeBase() const {
        return time_base;
    }
    AVRational GetFrameRate() const {
        return frame_rate;
    }

    AVStream* NewStream() {
        AVStream* stream = avformat_new_stream(format_context, nullptr);

        if (!stream) THROW_FFMPEG("Failed to create output stream");
        stream->time_base = GetTimeBase();

        return stream;
    }

    void WriteHeaders() {
        AVDictionary* options = nullptr;
        if (avformat_write_header(format_context, &options)) 
            THROW_FFMPEG("Failed to write format header");

    }

    void WritePacket(AVPacket* packet) {
        av_interleaved_write_frame(format_context, packet);
        av_packet_unref(packet);
    }

private:
    AVFormatContext* format_context;
    std::unique_ptr<FfmpegOutput> output;
    AVRational time_base;
    AVRational frame_rate;
};