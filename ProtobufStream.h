#pragma once
#include <mutex>
#include "Logger.h"

template<typename InputMessage, typename OutputMessage>
class ProtobufStream {
public:
    ProtobufStream(boost::asio::ip::tcp::socket&& _socket, std::function<void(const InputMessage&)> _message_handler) :
                   socket(std::move(_socket)), message_handler(_message_handler) {
        // Begin reading messages
        ReadMessage();
    }

    void WriteMessage(const OutputMessage& message) {
        std::scoped_lock lock(socket_mutex);

        size_t message_size = message.ByteSizeLong();
        if (write_buffer.size() < message_size + 4) write_buffer.resize(message_size + 4);

        message.SerializeToArray(write_buffer.data() + 4, write_buffer.size() - 4);
        uint32_t size = htonl(message_size);
        std::memcpy(write_buffer.data(), &size, 4);

        boost::asio::write(socket, boost::asio::buffer(write_buffer, message_size + 4));
    }

private:
    std::mutex socket_mutex;
    std::vector<char> write_buffer;
    std::vector<char> read_buffer;
    boost::asio::ip::tcp::socket socket;
    std::function<void(const InputMessage&)> message_handler;
    
    void AsyncRead(size_t size, const std::function<void(const boost::system::error_code&, std::size_t)>& handler) {
        std::scoped_lock lock(socket_mutex);
        if (read_buffer.size() < size) read_buffer.resize(size);
        boost::asio::async_read(socket, boost::asio::buffer(read_buffer, size), handler);
    }

    void ReadMessage() {
        AsyncRead(sizeof(int32_t), boost::bind(&ProtobufStream::HandleMessageLength, this,
                                               boost::asio::placeholders::error,
                                               boost::asio::placeholders::bytes_transferred));
    }

    void HandleMessageLength(const boost::system::error_code& ec, std::size_t size) {
        if (ec) {
            // TODO: Something failed, we should probably do something
            tlog << "Failed to read message length";
            exit(2);
        }

        uint32_t length;
        std::memcpy(&length, read_buffer.data(), sizeof(length));
        length = ntohl(length);
        // tlog << "Message length " << length << " " << htonl(length);

        AsyncRead(length, boost::bind(&ProtobufStream::HandleMessage, this,
                                      boost::asio::placeholders::error,
                                      boost::asio::placeholders::bytes_transferred));
    }

    void HandleMessage(const boost::system::error_code& ec, std::size_t size) {
        if (ec) {
            // TODO: Something failed, we should probably do something
            tlog << "Failed to read message";
            exit(2);
        }

        InputMessage message;
        message.ParseFromArray(read_buffer.data(), size);

        message_handler(message);

        // Repeat message reading
        ReadMessage();
    }
};
