#pragma once

#include <boost/asio.hpp>

#include "FfmpegInput.h"

class FfmpegNetworkInput : public FfmpegInput {
public:
    FfmpegNetworkInput(boost::asio::ip::tcp::socket _socket, size_t buffer_size = 4096) : socket(std::move(_socket)) {
        buffer = (uint8_t*) av_malloc(buffer_size);
        avio_context = avio_alloc_context(
            buffer,
            buffer_size,
            0,
            this,
            ReadData,
            nullptr,
            nullptr
        );
    }

    virtual ~FfmpegNetworkInput() {
        avio_context_free(&avio_context);
        av_free(buffer);
        socket.close();
        // TODO: Ensure thread safety
    }

    virtual AVIOContext* GetAvioContext() {
        return avio_context;
    }

private:
    static int ReadData(void* opaque, uint8_t* buffer, int buffer_size) {
        FfmpegNetworkInput& input = *reinterpret_cast<FfmpegNetworkInput*>(opaque);
        std::size_t data_read = boost::asio::read(input.socket, boost::asio::buffer(buffer, buffer_size));
        // TODO: Handle boost error
        return data_read;
    }

    boost::asio::ip::tcp::socket socket;
    uint8_t* buffer;
    AVIOContext* avio_context;
};