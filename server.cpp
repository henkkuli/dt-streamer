#include <boost/asio.hpp>
#include <vector>
#include <iostream>
#include <thread>
#include <string>
#include <chrono>
#include <thread>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>
#include <google/protobuf/message.h>

#include "control.grpc.pb.h"
#include "messages.pb.h"
#include "FfmpegNetworkOutput.h"
#include "FfmpegMuxer.h"
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

class Source {
public:
    Source(boost::asio::ip::tcp::socket _socket) :
           stream(std::move(_socket), boost::bind(&Source::HandleMessage, this, boost::placeholders::_1)) {
        
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

private:
    ProtobufStream<ClientData, ClientControl> stream;

    void HandleMessage(const ClientData& message) {
        tlog << "Received message";
    }
};

class Sink {
public:
    Sink(boost::asio::io_service& io_service, const std::string& address, uint16_t port) {
        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address::from_string(address), port);
        boost::asio::ip::tcp::socket socket(io_service);
        socket.connect(endpoint);

        muxer = std::make_shared<FfmpegMuxer>(av_guess_format("mpegts", nullptr, nullptr),
                                              std::make_unique<FfmpegNetworkOutput>(std::move(socket)),
                                              /* frame rate */ 60);
        encoder = FfmpegVideoEncoder::CreateEncoder("libx264", muxer, 1920, 1080);
        muxer->WriteHeaders();
    }

private:
    std::shared_ptr<FfmpegMuxer> muxer;
    std::unique_ptr<FfmpegVideoEncoder> encoder;
};

class Router {
public:
    Router(boost::asio::io_service& io_service, uint16_t port) :
        acceptor(io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)) {

    }

private:
    std::vector<std::unique_ptr<Sink>> sinks;
    std::vector<std::unique_ptr<Source>> sources;
    boost::asio::ip::tcp::acceptor acceptor;
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

    boost::asio::ip::tcp::acceptor acceptor(*io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 5000));

    std::vector<std::thread> worker_threads;
    for (int i = 0; i < 3; i++) {
        worker_threads.emplace_back(
            boost::bind(
                &WorkerThread, io_service
            )
        );
    }

    for (;;) {
        boost::asio::ip::tcp::socket source_socket(*io_service);

        acceptor.accept(source_socket);

        tlog << "Connected";

        Source source(std::move(source_socket));

        for (int i = 0; i < 3; i++) {
            using namespace std::chrono_literals;
            source.StartStream();
            std::this_thread::sleep_for(3s);
            source.StopStream();
            std::this_thread::sleep_for(3s);
        }
    }
    // Sink sink(io_service, "localhost", 5000);


    // ControlServer control_server;
    // grpc::ServerBuilder server_builder;
    // server_builder.AddListeningPort("0.0.0.0:5000", grpc::InsecureServerCredentials());
    // server_builder.RegisterService(&control_server);
    // auto server = server_builder.BuildAndStart();
    // std::cout << "Server started" << std::endl;
    // server->Wait();
    // std::cout << "Server stopped" << std::endl;
}
