#include "Util.h"

boost::asio::ip::tcp::socket connect_socket(std::shared_ptr<boost::asio::io_service> io_service,
                                            const std::string& address,
                                            uint16_t port) {
    boost::asio::ip::tcp::resolver resolver(*io_service);
    auto endpoints = resolver.resolve(address, std::to_string(port));
    boost::asio::ip::tcp::socket socket(*io_service);

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
            socket = boost::asio::ip::tcp::socket{*io_service};
        }
    }
    if (!connected) {
        tlog << "Failed to connect " << address << ":" << port;
        // Crash the program...
        // TODO: More elegant error handling
        exit(2);
    }

    return socket;
}