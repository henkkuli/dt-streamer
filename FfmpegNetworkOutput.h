#pragma once

#include <boost/asio.hpp>
extern "C" {
#include <libavformat/avformat.h>
}

#include "FfmpegOutput.h"

class FfmpegNetworkOutput : public FfmpegOutput {
public:
    FfmpegNetworkOutput(boost::asio::ip::tcp::socket _socket, size_t buffer_size = 4096) : socket(std::move(_socket)) {
        buffer = (uint8_t*) av_malloc(buffer_size);
        avio_context = avio_alloc_context(
            buffer,
            buffer_size,
            1,
            this,
            nullptr,
            WriteData,
            nullptr
        );
    }

    virtual ~FfmpegNetworkOutput() {
        avio_context_free(&avio_context);
        av_free(buffer);
        socket.close();
        // TODO: Ensure thread safety
    }

    virtual AVIOContext* GetAvioContext() {
        return avio_context;
    }

private:
    static int WriteData(void* opaque, uint8_t* buffer, int buffer_size) {
        FfmpegNetworkOutput& output = *reinterpret_cast<FfmpegNetworkOutput*>(opaque);
        boost::asio::write(output.socket, boost::asio::buffer(buffer, buffer_size));
        // TODO: Handle boost error
        return 0;
    }

    boost::asio::ip::tcp::socket socket;
    uint8_t* buffer;
    AVIOContext* avio_context;
};
