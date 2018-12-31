#pragma once

#include <mutex>
#include <sstream>

// Log levels. When changin these, remember to change log level names in Logger.cpp
enum log_level {
    ERROR = 0,
    WARN,
    NOTICE,
    INFO,
    DEBUG
};

class LogTemporary {
public:
    LogTemporary(log_level level, const char* file, unsigned li);
    LogTemporary(LogTemporary&&) = default;

    ~LogTemporary();

    template<typename T>
    LogTemporary& operator<<(T value) {
        stream << value;
        return *this;
    }

private:
    static std::mutex log_mutex;
    std::stringstream stream;
    bool enabled;
};

#define LOG(level) log_internal(level, __FILE__, __LINE__)
LogTemporary log_internal(log_level level, const char* file, unsigned line);

void set_log_level(log_level level);
void set_log_level(std::string level);

void ffmpeg_log_callback(void* ptr, int level, const char* format, va_list vl);
