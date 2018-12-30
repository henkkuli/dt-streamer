#pragma once

#include <boost/asio.hpp>
#include "Logger.h"

boost::asio::ip::tcp::socket connect_socket(std::shared_ptr<boost::asio::io_service> io_service,
                                            const std::string& address,
                                            uint16_t port);
