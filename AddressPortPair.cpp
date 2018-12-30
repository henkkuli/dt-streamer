#include "AddressPortPair.h"

#include <exception>

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
