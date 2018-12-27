#define BOOST_COROUTINES_NO_DEPRECATION_WARNING

#include <vector>
#include <iostream>
#include <thread>
#include <string>
#include <chrono>
#include <thread>
#include <deque>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/bind.hpp>
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>
#include <google/protobuf/message.h>

#include "control.grpc.pb.h"
#include "messages.pb.h"
#include "FfmpegDemuxer.h"
#include "FfmpegInput.h"
#include "FfmpegNetworkOutput.h"
#include "FfmpegMuxer.h"
#include "FfmpegVideoDecoder.h"
#include "FfmpegVideoEncoder.h"
#include "ProtobufStream.h"
#include "Logger.h"

class AsyncInput;

class AsyncInputInternal : public FfmpegInput {
public:
    AsyncInputInternal(AsyncInput* _async_input) : async_input(_async_input) {
    }
       
    virtual AVIOContext* GetAvioContext();

private:
    AsyncInput* async_input;
};

class AsyncInput {
public:
    AsyncInput(std::shared_ptr<boost::asio::io_service> _io_service, size_t buffer_size = 4096) : io_service(_io_service) {
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

    virtual ~AsyncInput() {
        avio_context_free(&avio_context);
        av_free(buffer);
        // TODO: Ensure thread safety
    }

    void SendData(const std::string& data) {
        std::scoped_lock lock(async_buffer_mutex);
        async_buffer.insert(async_buffer.end(), data.data(), data.data() + data.size());
    }

    std::unique_ptr<AsyncInputInternal> GetAsyncInput() {
        return std::make_unique<AsyncInputInternal>(this);
    }

    void SetYielder(boost::asio::yield_context* _yield) {
        yield = _yield;
    }

private:
    static int ReadData(void* opaque, uint8_t* buffer, int buffer_size) {
        // TODO: Lock for yield
        AsyncInput& input = *reinterpret_cast<AsyncInput*>(opaque);
        if (!input.yield) return 0;

        // Loop until all data has been received
        while (true) {
            {
                std::scoped_lock lock(input.async_buffer_mutex);
                if (input.async_buffer.size() >= size_t(buffer_size)) {
                    std::copy(input.async_buffer.begin(), input.async_buffer.begin() + buffer_size, buffer);
                    // Consume the buffer from the queue
                    input.async_buffer.erase(input.async_buffer.begin(), input.async_buffer.begin() + buffer_size);
                    return buffer_size;
                }
            }
            input.io_service->post(*input.yield);
        }
    }

    std::shared_ptr<boost::asio::io_service> io_service;
    boost::asio::yield_context* yield = nullptr;
    uint8_t* buffer;
    std::thread thread;
    std::deque<char> async_buffer;
    std::mutex async_buffer_mutex;
    AVIOContext* avio_context;

    friend class AsyncInputInternal;
};

AVIOContext* AsyncInputInternal::GetAvioContext() {
    return async_input->avio_context;
}

class Sink;

class Source {
public:
    Source(std::shared_ptr<boost::asio::io_service> _io_service, boost::asio::ip::tcp::socket _socket) :
           io_service(_io_service),
           work(*io_service),
           address(_socket.remote_endpoint().address().to_string()),
           stream(std::move(_socket), boost::bind(&Source::HandleMessage, this, boost::placeholders::_1)) {
        async_input = std::make_unique<AsyncInput>(io_service);
        demuxer = std::make_shared<FfmpegDemuxer>(av_find_input_format("mpegts"), async_input->GetAsyncInput());

        // Start decoding in a coroutine
        boost::asio::spawn(*io_service, boost::bind(&Source::DecodeAll, this, boost::placeholders::_1));
    }

    void StartStream() {
        tlog << "Starting";
        ClientControl message;
        message.mutable_start_stream();
        stream.WriteMessage(message);
    }

    void StopStream() {
        tlog << "Stopping";
        ClientControl message;
        message.mutable_stop_stream();
        stream.WriteMessage(message);
    }

    void ConnectTo(std::shared_ptr<Sink> sink);

    void DetachFrom(Sink* sink);

    std::string Name() {
        // TODO: Find a real name instead of address
        return address;
    }

private:
    std::shared_ptr<boost::asio::io_service> io_service;
    boost::asio::io_service::work work;
    std::string address;
    ProtobufStream<ClientData, ClientControl> stream;
    std::unique_ptr<AsyncInput> async_input;
    std::shared_ptr<FfmpegDemuxer> demuxer;
    std::unique_ptr<FfmpegVideoDecoder> decoder;
    std::set<std::shared_ptr<Sink>> target_sinks;
    std::mutex target_sinks_mutex;

    void HandleMessage(const ClientData& message) {
        async_input->SendData(message.payload());
    }

    void DecodeFrame() {
        if (!decoder) {
            decoder = std::unique_ptr<FfmpegVideoDecoder>(demuxer->FindVideoStream());
            if (decoder) {
                decoder->OnFrame(boost::bind(&Source::OnFrame, this, boost::placeholders::_1));
            }
        } else {
            demuxer->DemuxNextFrame();
        }
    }

    void DecodeAll(boost::asio::yield_context yield) {
        // Async input needs a yielder before it can be used
        async_input->SetYielder(&yield);
        while (1) {
            DecodeFrame();
            // Yield after every group of frames for outher stuff
            io_service->post(yield);
        }
    }

    void OnFrame(AVFrame* frame);
};

class Sink {
public:
    Sink(boost::asio::io_service& io_service, const std::string& address, uint16_t port) {
        boost::asio::ip::tcp::resolver resolver(io_service);
        auto endpoints = resolver.resolve(address, std::to_string(port));
        // boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address::from_string(address), port);
        boost::asio::ip::tcp::socket socket(io_service);
        // socket.connect(endpoint);
        bool connected = false;
        for (auto& endpoint : endpoints) {
            tlog << "Connecting to " << endpoint.endpoint().address() << ":" << endpoint.endpoint().port();
            boost::system::error_code ec;
            socket.connect(endpoint.endpoint(), ec);
            if (!ec) {
                connected = true;
                break;
            } else {
                tlog << ec.message();
                // Reset the socket
                socket = boost::asio::ip::tcp::socket{io_service};
            }
        }
        if (!connected) {
            tlog << "Failed to connect " << address << ":" << port;
            // Crash the program...
            // TODO: More elegant error handling
            exit(2);
        }

        muxer = std::make_shared<FfmpegMuxer>(av_guess_format("mpegts", nullptr, nullptr),
                                              std::make_unique<FfmpegNetworkOutput>(std::move(socket)),
                                              /* frame rate */ 30);
        encoder = FfmpegVideoEncoder::CreateEncoder("libx264", muxer, 1920, 1080);
        muxer->WriteHeaders();
    }

    void DetachFromSource() {
        if (!source) return;
        auto detached_source = source;
        source = nullptr;
        detached_source->DetachFrom(this);
    }

    void SendFrame(AVFrame* frame) {
        // TODO: Scaling
        AVFrame* target = encoder->GetNextFrame();
        av_frame_copy(target, frame);
        target->pts = frame_number++;
        tlog << target->width << "x" << target->height << " "
             << frame->width << "x" << frame->height;
        encoder->SwapFrames();
        encoder->WriteFrame();
    }

private:
    std::shared_ptr<FfmpegMuxer> muxer;
    std::unique_ptr<FfmpegVideoEncoder> encoder;
    Source* source = nullptr;
    int64_t frame_number = 0;

    friend class Source;
};

void Source::OnFrame(AVFrame* frame) {
    std::scoped_lock lock(target_sinks_mutex);
    for (auto target : target_sinks) {
        target->SendFrame(frame);
    }
}

void Source::ConnectTo(std::shared_ptr<Sink> sink) {
        std::scoped_lock lock(target_sinks_mutex);
        target_sinks.insert(sink);
        sink->source = this;
        StartStream();
    }

void Source::DetachFrom(Sink* sink) {
    std::scoped_lock lock(target_sinks_mutex);
    for (auto it = target_sinks.begin(); it != target_sinks.end(); it++) {
        if (it->get() == sink) {
            target_sinks.erase(it);
            if (target_sinks.empty()) StopStream();
            return;
        }
    }
}

class Router {
public:
    Router(std::shared_ptr<boost::asio::io_service> _io_service, uint16_t port) :
        io_service(_io_service),
        acceptor(*io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)) {
    }

    void StartAccepting() {
        acceptor.async_accept(std::bind(&Router::AcceptConnection, this,
                                        std::placeholders::_1, std::placeholders::_2));;
    }

    void ConnectSourceToSink(size_t source, size_t sink) {
        if (source >= sources.size()) {
            throw std::invalid_argument("source");
        }
        if (sink >= sinks.size()) {
            throw std::invalid_argument("sink");
        }
        sinks[sink]->DetachFromSource();
        sources[source]->ConnectTo(sinks[sink]);
    }

    std::vector<std::shared_ptr<Source>> ListSources() const {
        return sources;
    }

    void AddSink(std::shared_ptr<Sink> sink) {
        sinks.push_back(sink);
    }

    void DetachSink(size_t sink) {
        if (sink >= sinks.size()) {
            throw std::invalid_argument("sink");
        }
        sinks[sink]->DetachFromSource();
    }

private:
    std::shared_ptr<boost::asio::io_service> io_service;
    std::vector<std::shared_ptr<Sink>> sinks;
    std::vector<std::shared_ptr<Source>> sources;
    boost::asio::ip::tcp::acceptor acceptor;

    void AcceptConnection(const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
        if (ec) {
            tlog << "Error accepting a connection: " << ec.message();
            // Try still again
            StartAccepting();
            return;
        }
        tlog << "Connection accepted";
        auto source = std::make_shared<Source>(io_service, std::move(socket));
        sources.push_back(source);
        // Accept more
        StartAccepting();
    }
};

class ControlServer final : public Control::Service {
public:
    ControlServer(std::shared_ptr<Router> _router) : router(_router) {
    }

    grpc::Status ListSources(grpc::ServerContext* context, const ListSourcesRequest* request,
                             ListSourcesResponse* response) override {
        tlog << "Listing sources";
        for (auto source : router->ListSources()) {
            response->add_sources()->set_name(source->Name());
        }
        return grpc::Status::OK;
    }

    grpc::Status ConnectSourceToSink(grpc::ServerContext* context, const ConnectSourceToSinkRequest* request,
                                     ConnectSourceToSinkResponse* response) override {
        tlog << "Connecting source " << request->source() << " to sink " << request->sink();
        router->ConnectSourceToSink(request->source(), request->sink());
        return grpc::Status::OK;
    }

    grpc::Status DetachSink(grpc::ServerContext* context, const DetachSinkRequest* request,
                            DetachSinkResponse* response) override {
        tlog << "Detaching sink " << request->sink();
        router->DetachSink(request->sink());
        return grpc::Status::OK;
    }

private:
    std::shared_ptr<Router> router;
};

void WorkerThread(std::shared_ptr<boost::asio::io_service> io_service) {
    while (1) {
        tlog << "Starting runner";
        io_service->run();
        tlog << "Stopping runner";
    }
}

void usage(const char* program) {
    std::cerr << program << " control_port data_port (address port)*\n"
              << "    control_port  port being listened for control trafic (1 - 65535)\n"
              << "    data_port     port for clients to connect to (1 - 65535)\n"
              << "    address       addresses of targets\n"
              << "    port          ports of targets\n";
}

int main(int argc, char** argv) {
    uint16_t control_port, data_port;
    std::vector<std::pair<std::string, uint16_t>> target_addresses;
    if (argc < 5) {
        usage(argv[0]);
        return 1;
    }
    try {
        int control_port_int = std::stoi(argv[1]);
        int data_port_int = std::stoi(argv[2]);

        if (control_port_int < 1 || control_port_int > 65535) throw std::out_of_range("Control port out of range");
        if (data_port_int < 1 || data_port_int > 65535) throw std::out_of_range("Data port out of range");

        control_port = control_port_int;
        data_port = data_port_int;

        char** arg = argv + 3;
        argc -= 3;
        if (argc % 2 != 0) throw std::out_of_range("Wrong number of arguments");
        for (int i = 0; i < argc; i += 2) {
            int port = std::stoi(arg[i+1]);
            if (port < 1 || port > 65535) throw std::out_of_range("Target port out of range");

            target_addresses.push_back({
                arg[i],
                port
            });
        }
    } catch (std::invalid_argument& e) {
        usage(argv[0]);
        return 1;
    } catch (std::out_of_range& e) {
        usage(argv[0]);
        return 1;
    }

    auto io_service = std::make_shared<boost::asio::io_service>();
    boost::asio::io_service::work work(*io_service);
    auto router = std::make_shared<Router>(io_service, data_port);

    // TODO: Log using the tlog
    // av_log_set_callback(nullptr);

    for (auto target : target_addresses) {
        router->AddSink(std::make_shared<Sink>(*io_service, target.first, target.second));
    }

    // Start the control server
    ControlServer control_server(router);
    grpc::ServerBuilder server_builder;
    server_builder.AddListeningPort("0.0.0.0:" + std::to_string(control_port), grpc::InsecureServerCredentials());
    server_builder.RegisterService(&control_server);
    auto server = server_builder.BuildAndStart();

    router->StartAccepting();

    // Let's launch io_service workers in a separate thread(s). For some reason grpc server and io_service can't be run
    // from the same thread. Thus don't run io_service->run() from the main thread.
    std::vector<std::thread> worker_threads;
    for (int i = 0; i < 1; i++) {
        worker_threads.emplace_back(
            boost::bind(
                &WorkerThread, io_service
            )
        );
    }

    // Let's wait for all of the services. We should never run past this point.
    for (auto &thread : worker_threads) {
        thread.join();
    }
    server->Wait();
}
