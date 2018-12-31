#pragma once

#include <exception>
#include <string>
#include <vector>

#include "Logger.h"

class FfmpegException : public std::exception {
public:
    FfmpegException(const std::string& _message) : message(_message) {}

    const char* what() const throw () {
        return message.c_str();
    }

private:
    std::string message;
};

constexpr size_t ERROR_BUFFER_SIZE = 1024;
#define STRINGIFY(x) #x
#define TO_STRING(x) STRINGIFY(x)
#define THROW_FFMPEG(message) do { \
    LOG(ERROR) << "Ffmpeg error: " << message; \
    throw FfmpegException(std::string(__FILE__ ":" TO_STRING(__LINE__) ": ") + message); \
} while(0)

#define THROW_ON_AV_ERROR(expr) do { \
    int error_number_##__LINE__ = expr; \
    if (error_number_##__LINE__  < 0) { \
        /* Get the error message */ \
        std::vector<char> buffer_##__LINE__(ERROR_BUFFER_SIZE); \
        av_make_error_string(buffer_##__LINE__.data(), ERROR_BUFFER_SIZE, error_number_##__LINE__); \
        THROW_FFMPEG(buffer_##__LINE__.data()); \
    } \
} while (false)
