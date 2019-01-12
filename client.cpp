#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/program_options.hpp>
extern "C" {
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}
#include "pwd.h"

#include "AddressPortPair.h"
#include "FfmpegDemuxer.h"
#include "FfmpegMuxer.h"
#include "FfmpegOutput.h"
#include "FfmpegVideoDecoder.h"
#include "FfmpegVideoEncoder.h"
#include "ProtobufStream.h"
#include "messages.pb.h"
#include "Logger.h"
#include "Util.h"

// This must be here because it redefines Status, which breaks protobuf
#include <X11/Xlib.h>

namespace po = boost::program_options;

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
        ClientData message;
        auto data = message.mutable_data();
        data->mutable_payload()->assign(reinterpret_cast<char*>(buffer), buffer_size);
        output.stream->WriteMessage(message);
        return 0;
    }

    ProtobufStream<ClientControl, ClientData>* stream;
    uint8_t* buffer;
    AVIOContext* avio_context;
};

std::string get_hostname() {
    char hostname[HOST_NAME_MAX + 1];
    gethostname(hostname, HOST_NAME_MAX + 1);
    hostname[HOST_NAME_MAX] = '\0';
    std::string name = hostname;
    return name;
}

std::string get_username() {
    uid_t uid = getuid();
    passwd* pw = getpwuid(uid);
    if (!pw) return "<unknown>";
    std::string username = pw->pw_name;
    return username;
}

class Connection {
public:
    Connection(std::shared_ptr<boost::asio::io_service> _io_service, const std::string& address, uint16_t port,
               int width, int height) :
               io_service(_io_service),
               work(*io_service),
               stream(connect_socket(io_service, address, port), boost::bind(&Connection::HandleMessage, this,
                                                                             boost::placeholders::_1),
                      []() { /* TODO: Do something on close */ exit(2); }) {
        // Create video output
        auto output = std::make_unique<ProtobufOutput>(&stream);
        auto muxer = std::make_shared<FfmpegMuxer>(av_guess_format("mpegts", nullptr, nullptr), std::move(output), 30);
        encoder = FfmpegVideoEncoder::CreateEncoder(io_service, "libx264", muxer, 1920, 1080);
        muxer->WriteHeaders();

        // Create screen capture input
        demuxer = std::make_unique<FfmpegX11grabDemuxer>(std::getenv("DISPLAY"), width, height);
        decoder = demuxer->FindVideoStream();
        decoder->OnFrame(boost::bind(&Connection::OnFrame, this, boost::placeholders::_1));

        // Scaler
        scaling_context = sws_getContext(width, height, AV_PIX_FMT_BGR0, 1920, 1080, AV_PIX_FMT_YUV420P, 0,
                                         nullptr, nullptr, nullptr);

        // Start decoding
        DecodeFrame();

        // Send hello message
        ClientData hello_message;
        auto hello = hello_message.mutable_hello();
        hello->set_hostname(get_hostname());
        hello->set_username(get_username());
        stream.WriteMessage(hello_message);
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
            LOG(INFO) << "Starting stream";
            std::scoped_lock lock(streaming_mutex);
            streaming = true;
        }
        if (message.has_stop_stream()) {
            LOG(INFO) << "Stopping stream";
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
    po::options_description description("dt-streamer client");
    po::positional_options_description positional_description;
    description.add_options()
        ("help", "Show this help")
        ("log-level", po::value<std::string>()->default_value("INFO"),
         "Logging level. ERROR, WARNING, NOTICE, INFO, DEBUG.")
        ("server", po::value<AddressPortPair>(), "Address of the dt-streamer server")
    ;
    positional_description.add("server", 1);

    po::variables_map args;
    po::store(po::command_line_parser(argc, argv)
              .options(description)
              .positional(positional_description)
              .run(),
              args);
    po::notify(args);


    if (args.count("help") || !args.count("server")) {
        std::cout << description << std::endl;
        return 1;
    }

    try {
        set_log_level(args["log-level"].as<std::string>());
    } catch (std::invalid_argument& e) {
        std::cout << "Invalid log level" << std::endl;
        std::cout << description << std::endl;
        return 1;
    }

    const AddressPortPair server = args["server"].as<AddressPortPair>();

    av_log_set_callback(ffmpeg_log_callback);

    // Let's connect to X to get the screen size
    Display* display = XOpenDisplay(nullptr);       // Use the default display from DISPLAY environment variable

    int width = XDisplayWidth(display, 0);
    int height = XDisplayHeight(display, 0);
    LOG(INFO) << "Display size is " << width << "x" << height;

    XCloseDisplay(display);

    // Register all devices for ffmpeg, especially x11grab
    avdevice_register_all();

    auto io_service = std::make_shared<boost::asio::io_service>();

    Connection connection(io_service, server.address, server.port, width, height);

    io_service->run();
}
