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

class ControlServer final : public Control::Service {
public:
    grpc::Status ListSources(grpc::ServerContext* context, const ListSourcesRequest* request,
                             ListSourcesResponse* response) override {
        std::cout << "Listing sources" << std::endl;
        response->add_sources()->set_name("A source");
        return grpc::Status::OK;
    }
};

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

    void ConnectTo(std::shared_ptr<Sink> sink) {
        target_sink = sink;
        if (sink) StartStream();
    }

private:
    std::shared_ptr<boost::asio::io_service> io_service;
    boost::asio::io_service::work work;
    ProtobufStream<ClientData, ClientControl> stream;
    std::unique_ptr<AsyncInput> async_input;
    std::shared_ptr<FfmpegDemuxer> demuxer;
    std::unique_ptr<FfmpegVideoDecoder> decoder;
    std::shared_ptr<Sink> target_sink;

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
        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address::from_string(address), port);
        boost::asio::ip::tcp::socket socket(io_service);
        socket.connect(endpoint);

        muxer = std::make_shared<FfmpegMuxer>(av_guess_format("mpegts", nullptr, nullptr),
                                              std::make_unique<FfmpegNetworkOutput>(std::move(socket)),
                                              /* frame rate */ 30);
        encoder = FfmpegVideoEncoder::CreateEncoder("libx264", muxer, 1920, 1080);
        muxer->WriteHeaders();
    }

    void DetachFromSource() {
        if (!source) return;
        source->ConnectTo(nullptr);
        source = nullptr;
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
    std::shared_ptr<Source> source;
    int64_t frame_number = 0;
};

void Source::OnFrame(AVFrame* frame) {
    if (target_sink) {
        target_sink->SendFrame(frame);
    }
}

class Router {
public:
    Router(std::shared_ptr<boost::asio::io_service> _io_service, uint16_t port) :
        io_service(_io_service),
        acceptor(*io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)) {

        sinks.push_back(std::make_shared<Sink>(*io_service, "127.0.0.1", 5001));
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

private:
    std::shared_ptr<boost::asio::io_service> io_service;
    std::vector<std::shared_ptr<Sink>> sinks;
    std::vector<std::shared_ptr<Source>> sources;
    boost::asio::ip::tcp::acceptor acceptor;

    void AcceptConnection(const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
        if (ec) {
            tlog << "Error accepting a connection: " << ec.message();
            return;
        }
        tlog << "Connection accepted";
        auto source = std::make_shared<Source>(io_service, std::move(socket));
        sources.push_back(source);

        // TODO: Remove this
        ConnectSourceToSink(0, 0);
    }
};

void WorkerThread(std::shared_ptr<boost::asio::io_service> io_service) {
    while (1) {
        tlog << "Starting runner";
        io_service->run();
        tlog << "Stopping runner";
    }
}

int main(int argc, char** argv) {
    auto io_service = std::make_shared<boost::asio::io_service>();
    boost::asio::io_service::work work(*io_service);

    // TODO: Log using the tlog
    // av_log_set_callback(nullptr);

    auto router = std::make_shared<Router>(io_service, 5000);

    router->StartAccepting();

    std::vector<std::thread> worker_threads;
    // for (int i = 0; i < 3; i++) {
    //     worker_threads.emplace_back(
    //         boost::bind(
    //             &WorkerThread, io_service
    //         )
    //     );
    // }
    io_service->run();
}
