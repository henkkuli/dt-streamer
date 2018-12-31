#include "Logger.h"

#include <algorithm>
#include <atomic>
#include <boost/date_time.hpp>
#include <iomanip>
#include <iostream>
#include <thread>

static const std::string log_level_names[] = {
    "ERROR  ",
    "WARNING",
    "NOTICE ",
    "INFO   ",
    "DEBUG  ",
};

static const std::string log_level_color_codes[] = {
    "\x1b[38;5;9m",
    "\x1b[38;5;1m",
    "\x1b[38;5;2m",
    "\x1b[38;5;7m",
    "\x1b[38;5;3m",
};

static const std::string DEFAULT_COLOR = "\e[0m";

static std::atomic<log_level> current_log_level;
std::mutex LogTemporary::log_mutex;

LogTemporary::LogTemporary(log_level level, const char* file, unsigned line) : enabled(level <= current_log_level) {
    if (!enabled) return;

    static std::locale locale (std::cout.getloc(), new boost::posix_time::time_facet("%Y-%m-%d %H:%M:%s"));
    stream.imbue(locale);

    auto now = boost::posix_time::microsec_clock::local_time();
    auto thread_id = std::this_thread::get_id();

    stream << "[";
    stream << now << " ";
    stream << log_level_color_codes[level] << log_level_names[level] << DEFAULT_COLOR << " ";
    stream << "@ 0x" << std::hex << std::setw(2 * sizeof(thread_id))
           << std::setfill('0') << thread_id << std::setfill(' ') << std::dec << " ";
    stream << file << ":" << line;
    stream << "] ";
                  
}

LogTemporary::~LogTemporary() {
    if (!enabled) return;
    std::scoped_lock lock(LogTemporary::log_mutex);
    std::cout << stream.str() << std::endl;
}

LogTemporary log_internal(log_level level, const char* file, unsigned line) {
    LogTemporary tmp(level, file, line);
    return tmp;
}

void set_log_level(log_level level) {
    current_log_level = level;
}

static bool log_level_compare(std::string user, std::string real) {
    if (user.size() > real.size()) return false;

    for (size_t i = 0; i < user.size(); i++) {
        if (user[i] != real[i]) return false;
    }

    return true;
}

void set_log_level(std::string level) {
    std::transform(level.begin(), level.end(), level.begin(), ::tolower);

    if (log_level_compare(level, "error")) set_log_level(ERROR);
    else if (log_level_compare(level, "warning")) set_log_level(WARN);
    else if (log_level_compare(level, "notice")) set_log_level(NOTICE);
    else if (log_level_compare(level, "info")) set_log_level(INFO);
    else if (log_level_compare(level, "debug")) set_log_level(DEBUG);
    else throw std::invalid_argument("level");
}
