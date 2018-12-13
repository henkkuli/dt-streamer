#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
extern "C" {
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}

#include "FfmpegDemuxer.h"
#include "FfmpegMuxer.h"
#include "FfmpegOutput.h"
#include "FfmpegVideoDecoder.h"
#include "FfmpegVideoEncoder.h"
#include "ProtobufStream.h"
#include "messages.pb.h"
#include "Logger.h"

class ProtobufOutput : public FfmpegOutput {
public:
    ProtobufOutput(ProtobufStream<ClientControl, ClientData>* _stream, size_t buffer_size = 4096) : stream(_stream) {
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

    ~ProtobufOutput() {
        avio_context_free(&avio_context);
        av_free(buffer);
    }

    virtual AVIOContext* GetAvioContext() {
        return avio_context;
    }

private:
    static int WriteData(void* opaque, uint8_t* buffer, int buffer_size) {
        ProtobufOutput& output = *reinterpret_cast<ProtobufOutput*>(opaque);
        ClientData data;
        data.mutable_payload()->assign(reinterpret_cast<char*>(buffer), buffer_size);
        output.stream->WriteMessage(data);
        return 0;
    }

    ProtobufStream<ClientControl, ClientData>* stream;
    uint8_t* buffer;
    AVIOContext* avio_context;
};

boost::asio::ip::tcp::socket connect_socket(std::shared_ptr<boost::asio::io_service> io_service,
                                            const std::string& address,
                                            uint16_t port) {
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address::from_string(address), port);
    boost::asio::ip::tcp::socket socket(*io_service);
    socket.connect(endpoint);
    return socket;
}

class Connection {
public:
    Connection(std::shared_ptr<boost::asio::io_service> _io_service, const std::string& address, uint16_t port) :
               io_service(_io_service),
               work(*io_service),
               stream(connect_socket(io_service, address, port), boost::bind(&Connection::HandleMessage, this,
                                                                             boost::placeholders::_1)) {
        // Create video output
        auto output = std::make_unique<ProtobufOutput>(&stream);
        auto muxer = std::make_shared<FfmpegMuxer>(av_guess_format("mpegts", nullptr, nullptr), std::move(output), 30);
        encoder = FfmpegVideoEncoder::CreateEncoder("libx264", muxer, 1920, 1080);
        muxer->WriteHeaders();

        // Create screen capture input
        demuxer = std::make_unique<FfmpegDemuxer>(av_find_input_format("x11grab"), ":0.0");
        decoder = demuxer->FindVideoStream();
        decoder->OnFrame(boost::bind(&Connection::OnFrame, this, boost::placeholders::_1));

        // Scaler
        scaling_context = sws_getContext(1920, 1080, AV_PIX_FMT_BGR0, 1920, 1080, AV_PIX_FMT_YUV420P, 0,
                                         nullptr, nullptr, nullptr);

        // Start decoding
        DecodeFrame();
    }

private:
    std::shared_ptr<boost::asio::io_service> io_service;
    boost::asio::io_service::work work;
    ProtobufStream<ClientControl, ClientData> stream;
    std::unique_ptr<FfmpegVideoEncoder> encoder;
    std::unique_ptr<FfmpegDemuxer> demuxer;
    FfmpegVideoDecoder* decoder;
    SwsContext* scaling_context;
    std::mutex streaming_mutex;
    bool streaming = false;
    
    void HandleMessage(const ClientControl& message) {
        if (message.has_start_stream()) {
            tlog << "Starting stream";
            std::scoped_lock lock(streaming_mutex);
            streaming = true;
        }
        if (message.has_stop_stream()) {
            tlog << "Stopping stream";
            std::scoped_lock lock(streaming_mutex);
            streaming = false;
        }
    }

    void OnFrame(AVFrame* frame) {
        {
            std::scoped_lock lock(streaming_mutex);
            if (!streaming) return;
        }

        static int64_t frame_number = 0;
        AVFrame* target_frame = encoder->GetNextFrame();
        sws_scale(scaling_context, frame->data, frame->linesize, 0, frame->height, target_frame->data, target_frame->linesize);
        target_frame->pts = frame_number++;
        encoder->SwapFrames();
        encoder->WriteFrame();
    }

    void DecodeFrame() {
        demuxer->DemuxNextFrame();
        io_service->post(boost::bind(&Connection::DecodeFrame, this));
    }

    friend class ProtobufOutput;
};

void usage(const char* program) {
    std::cerr << program << " address port\n"
              << "    address  address of the server\n"
              << "    port     port of the server (1 - 65535)\n";
}

int main(int argc, char** argv) {
    std::string address;
    uint16_t port;

    // Parse the arguments
    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }
    try {
        address = argv[1];
        int port_int = std::stoi(argv[2]);
        if (port_int < 0 || port_int > 65535) throw std::out_of_range("Port out of range");
        port = port_int;
    } catch (std::invalid_argument& e) {
        usage(argv[0]);
        return 1;
    } catch (std::out_of_range& e) {
        usage(argv[0]);
        return 1;
    }

    // Register all devices for ffmpeg, especially x11grab
    avdevice_register_all();

    auto io_service = std::make_shared<boost::asio::io_service>();

    Connection connection(io_service, address, port);

    io_service->run();
}
