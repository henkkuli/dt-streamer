#pragma once

#include <iostream>
#include <string>

struct AddressPortPair {
    std::string address;
    uint16_t port;
};

std::istream& operator>>(std::istream& stream, AddressPortPair& result);
