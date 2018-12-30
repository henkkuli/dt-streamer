#define BOOST_COROUTINES_NO_DEPRECATION_WARNING

#include <atomic>
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
#include <boost/program_options.hpp>
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

namespace po = boost::program_options;

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
        uint8_t* buffer = (uint8_t*) av_malloc(buffer_size);
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
        av_free(avio_context->buffer);
        avio_context_free(&avio_context);
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

    void Suspend() {
        suspended = true;
    }

private:
    static int ReadData(void* opaque, uint8_t* buffer, int buffer_size) {
        // TODO: Lock for yield
        AsyncInput& input = *reinterpret_cast<AsyncInput*>(opaque);
        if (!input.yield) return 0;

        // Loop until all data has been received
        while (!input.suspended) {
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

        // The execution has been suspended
        return AVERROR_EOF;
    }

    std::shared_ptr<boost::asio::io_service> io_service;
    boost::asio::yield_context* yield = nullptr;
    std::deque<char> async_buffer;
    std::mutex async_buffer_mutex;
    AVIOContext* avio_context;
    std::atomic<bool> suspended = false;

    friend class AsyncInputInternal;
};

AVIOContext* AsyncInputInternal::GetAvioContext() {
    return async_input->avio_context;
}

class Sink;

class Source : public std::enable_shared_from_this<Source> {
public:
    Source(std::shared_ptr<boost::asio::io_service> _io_service, boost::asio::ip::tcp::socket _socket, uint32_t _id,
           std::function<void()> on_close) :
           io_service(_io_service),
           work(*io_service),
           address(_socket.remote_endpoint().address().to_string()),
           id(_id),
           stream(std::move(_socket), boost::bind(&Source::HandleMessage, this, boost::placeholders::_1), on_close) {
        async_input = std::make_unique<AsyncInput>(io_service);
        demuxer = std::make_shared<FfmpegDemuxer>(av_find_input_format("mpegts"), async_input->GetAsyncInput());

        // Start decoding in a coroutine
        io_service->post([&]() {
            boost::asio::spawn(*io_service,
                               boost::bind(&Source::DecodeAll,
                                           shared_from_this(),
                                           boost::placeholders::_1)
                              );
        });
    }

    ~Source() {
        // Detaching from sinks is not required because they hold weak pointers
    }

    void StartStream() {
        io_service->dispatch([&]() {
            tlog << "Starting";
            ClientControl message;
            message.mutable_start_stream();
            stream.WriteMessage(message);
        });
    }

    void StopStream() {
        io_service->dispatch([&]() {
            tlog << "Stopping";
            ClientControl message;
            message.mutable_stop_stream();
            stream.WriteMessage(message);
        });
    }

    void ConnectTo(std::shared_ptr<Sink> sink);

    void DetachFrom(Sink* sink);

    std::string Name() const {
        // TODO: Find a real name instead of address
        return address;
    }

    uint32_t Id() const {
        return id;
    }

    void Suspend() {
        suspended = true;
        async_input->Suspend();
        // TODO: Detach all sinks
    }

private:
    std::shared_ptr<boost::asio::io_service> io_service;
    boost::asio::io_service::work work;
    const std::string address;
    const uint32_t id;
    const uint32_t reserved[3] = {0};       // TODO: Find out why this is needed
    ProtobufStream<ClientData, ClientControl> stream;
    std::unique_ptr<AsyncInput> async_input;
    std::shared_ptr<FfmpegDemuxer> demuxer;
    std::unique_ptr<FfmpegVideoDecoder> decoder;
    std::mutex target_sinks_mutex;
    std::set<std::shared_ptr<Sink>> target_sinks;
    std::atomic<bool> suspended = false;

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

    static void DecodeAll(std::shared_ptr<Source> source, boost::asio::yield_context yield) {
        tlog << "Starting decoding loop";
        // Async input needs a yielder before it can be used
        source->async_input->SetYielder(&yield);
        while (!source->suspended) {
            source->DecodeFrame();
            // Yield after every group of frames for outher stuff
            source->io_service->post(yield);
        }
        tlog << "Stopping decoding loop";
    }

    void OnFrame(AVFrame* frame);
};

class Sink {
public:
    Sink(boost::asio::io_service& io_service, const std::string& address, uint16_t port, uint32_t _id) :
         id(_id),
         name(address + ":" + std::to_string(port)) {
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
        if (auto detached_source = source.lock()) {
            source.reset();
            detached_source->DetachFrom(this);
        }
    }

    void SendFrame(AVFrame* frame) {
        // TODO: Scaling
        AVFrame* target = encoder->GetNextFrame();
        THROW_ON_AV_ERROR(av_frame_copy(target, frame));
        target->pts = frame_number++;
        tlog << target->width << "x" << target->height << " "
             << frame->width << "x" << frame->height;
        encoder->SwapFrames();
        encoder->WriteFrame();
    }

    std::string Name() const {
        return name;
    }

    uint32_t Id() const {
        return id;
    }

    std::weak_ptr<Source> GetSource() const {
        return source;
    }

private:
    const uint32_t id;
    const std::string name;
    std::shared_ptr<FfmpegMuxer> muxer;
    std::unique_ptr<FfmpegVideoEncoder> encoder;
    std::weak_ptr<Source> source;
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
        sink->source = weak_from_this();
        StartStream();
    }

void Source::DetachFrom(Sink* sink) {
    std::scoped_lock lock(target_sinks_mutex);
    for (auto it = target_sinks.begin(); it != target_sinks.end(); it++) {
        if (it->get() == sink) {
            target_sinks.erase(it);
            sink->DetachFromSource();
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

    void ConnectSourceToSink(uint32_t source_id, uint32_t sink_id) {
        std::scoped_lock lock(sinks_mutex, sources_mutex);
        auto source = sources.find(source_id);
        auto sink = sinks.find(sink_id);
        if (source == sources.end()) {
            throw std::invalid_argument("source");
        }
        if (sink == sinks.end()) {
            throw std::invalid_argument("sink");
        }
        sink->second->DetachFromSource();
        source->second->ConnectTo(sink->second);
    }

    std::vector<std::shared_ptr<Source>> ListSources() const {
        std::scoped_lock lock(sources_mutex);
        std::vector<std::shared_ptr<Source>> res;
        for (auto source : sources) res.push_back(source.second);
        return res;
    }

    std::vector<std::shared_ptr<Sink>> ListSinks() const {
        std::scoped_lock lock(sinks_mutex);
        std::vector<std::shared_ptr<Sink>> res;
        for (auto sink : sinks) res.push_back(sink.second);
        return res;
    }

    void AddSink(const std::string& address, uint16_t port) {
        std::scoped_lock lock(sinks_mutex);
        auto sink = std::make_shared<Sink>(*io_service, address, port, next_sink_id++);
        sinks[sink->Id()] = std::move(sink);
    }

    void DetachSink(uint32_t sink_id) {
        std::scoped_lock lock(sinks_mutex);
        auto sink = sinks.find(sink_id);
        if (sink == sinks.end()) {
            throw std::invalid_argument("sink");
        }
        sink->second->DetachFromSource();
    }

private:
    std::shared_ptr<boost::asio::io_service> io_service;
    mutable std::mutex sinks_mutex;
    std::map<uint32_t, std::shared_ptr<Sink>> sinks;
    uint32_t next_sink_id = 1;
    mutable std::mutex sources_mutex;
    std::map<uint32_t, std::shared_ptr<Source>> sources;
    uint32_t next_source_id = 1;
    boost::asio::ip::tcp::acceptor acceptor;

    void AcceptConnection(const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
        if (ec) {
            tlog << "Error accepting a connection: " << ec.message();
            // Try still again
            StartAccepting();
            return;
        }
        tlog << "Connection accepted";

        std::scoped_lock lock(sources_mutex);
        uint32_t source_id = next_source_id++;
        std::shared_ptr<Source> source = std::make_shared<Source>(io_service, std::move(socket), source_id,
            /* on_close */ std::bind([this](uint32_t id) {
                std::scoped_lock lock2(sources_mutex);
                auto src = sources[id];
                sources.erase(id);

                // Suspend the thread to flush the decoder
                src->Suspend();
            }, source_id));
        sources[source->Id()] = std::move(source);

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
            auto source_proto = response->add_sources();
            source_proto->set_id(source->Id());
            source_proto->set_name(source->Name());
        }
        return grpc::Status::OK;
    }

    grpc::Status ListSinks(grpc::ServerContext* context, const ListSinksRequest* request,
                             ListSinksResponse* response) override {
        tlog << "Listing sinks";
        for (auto sink : router->ListSinks()) {
            auto sink_proto = response->add_sinks();
            sink_proto->set_id(sink->Id());
            sink_proto->set_name(sink->Name());
            if (auto source = sink->GetSource().lock()) {
                sink_proto->set_source(source->Id());
            }
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

void checkPort(uint16_t port) {
    if (port < 1)
        throw std::invalid_argument("port");
}

struct AddressPortPair {
    std::string address;
    uint16_t port;
};

std::istream& operator>>(std::istream& stream, AddressPortPair& result) {
    std::string address;
    stream >> address;
    
    auto separator = address.find_last_of(':');
    if (separator == std::string::npos) throw std::invalid_argument("missing port");
    
    std::string port_str = address.substr(separator+1);
    int port = std::stoi(port_str);
    if (port < 1 || port > 65535) throw std::invalid_argument("port");

    address = address.substr(0, separator);

    result.address = address;
    result.port = port;
    
    return stream;
}

int main(int argc, char** argv) {
    po::options_description description("dt-streamer server");
    po::positional_options_description positional_description;
    description.add_options()
        ("help", "Show this help")
        ("threads", po::value<unsigned>()->default_value(1), "Number of threads for IO, excluding RPC")
        ("control-port", po::value<uint16_t>()->notifier(&checkPort)->default_value(6000), "Port for RPC connection")
        ("data-port", po::value<uint16_t>()->notifier(&checkPort)->default_value(5000), "Port for streaming clients")
        ("sink", po::value<std::vector<AddressPortPair>>(), "Address of a sink")
    ;
    positional_description.add("sink", -1);

    po::variables_map args;
    po::store(po::command_line_parser(argc, argv)
              .options(description)
              .positional(positional_description)
              .run(),
              args);
    po::notify(args);

    if (args.count("help") || !args.count("sink")) {
        std::cout << description << std::endl;
        return 1;
    }

    unsigned threads = args["threads"].as<unsigned>();
    uint16_t control_port = args["control-port"].as<uint16_t>();
    uint16_t data_port = args["data-port"].as<uint16_t>();
    std::vector<AddressPortPair> target_addresses = args["sink"].as<std::vector<AddressPortPair>>();

    auto io_service = std::make_shared<boost::asio::io_service>();
    boost::asio::io_service::work work(*io_service);
    auto router = std::make_shared<Router>(io_service, data_port);

    // TODO: Log using the tlog
    // av_log_set_callback(nullptr);

    for (auto target : target_addresses) {
        router->AddSink(target.address, target.port);
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
    for (unsigned i = 0; i < threads; i++) {
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
