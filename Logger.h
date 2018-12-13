#pragma once
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>

class LogTemporary {
public:
    LogTemporary() : lock(log_mutex) {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::cout << "[" << std::put_time(std::gmtime(&now), "%F %T")
                  << " @ " << std::this_thread::get_id() << "] ";
    }
    LogTemporary(LogTemporary&& tmp) : lock(std::move(tmp.lock)) {
    }
    ~LogTemporary() {
        std::cout << std::endl;
    }

    template<typename T>
    LogTemporary& operator<<(T value) {
        std::cout << value;
        return *this;
    }

private:
    static std::mutex log_mutex;
    std::unique_lock<std::mutex> lock;
};

class Log {
public:
    template<typename T>
    LogTemporary operator<<(T value) {
        LogTemporary tmp;
        tmp << value;
        return tmp;
    }
};

extern Log tlog;
