#include "Logger.h"

#include <algorithm>
#include <atomic>
#include <boost/date_time.hpp>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>
extern "C" {
#include <libavutil/avutil.h>
}

static const std::string log_level_names[] = {
    "ERROR  ",
    "WARNING",
    "NOTICE ",
    "INFO   ",
    "DEBUG  ",
    "TRACE  ",
};

static const std::string log_level_color_codes[] = {
    "\e[38;5;9m",
    "\e[38;5;1m",
    "\e[38;5;2m",
    "\e[38;5;7m",
    "\e[38;5;3m",
    "\e[38;5;5m",
};

static const std::string DEFAULT_COLOR = "\e[0m";

static std::atomic<log_level> current_log_level;
std::mutex LogTemporary::log_mutex;

static void output_time(std::ostream& out, const boost::posix_time::ptime& ptime) {
    auto date = ptime.date();
    auto time = ptime.time_of_day();
    out << std::setfill('0');
    out << std::setw(4) << date.year()
        << "-" << std::setw(2) << date.month().as_number()
        << "-" << std::setw(2) << date.day() << " ";
    out << std::setw(2) << time.hours() << ":"
        << std::setw(2) << time.minutes() << ":"
        << std::setw(2) << time.seconds() << "."
        << std::setw(time.num_fractional_digits()) << time.fractional_seconds();
}

LogTemporary::LogTemporary(log_level level, const char* file, unsigned line) : enabled(level <= current_log_level) {
    if (!enabled) return;

    auto now = boost::posix_time::microsec_clock::local_time();
    auto thread_id = std::this_thread::get_id();

    stream << "[";
    output_time(stream, now);
    stream << " ";
    stream << log_level_color_codes[level] << log_level_names[level] << DEFAULT_COLOR << " ";
    stream << "@ 0x" << std::hex << std::setw(2 * sizeof(thread_id))
           << std::setfill('0') << thread_id << std::setfill(' ') << std::dec << " ";
    stream << file;
    if (line > 0) stream << ":" << line;
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
    else if (log_level_compare(level, "trace")) set_log_level(TRACE);
    else throw std::invalid_argument("level");
}

void ffmpeg_log_callback(void* ptr, int level, const char* format, va_list vl) {
    AVClass* avc = ptr ? *(AVClass**)ptr : nullptr;

    // Copy the argument list
    va_list vl2;

    // Format the string into a temporary vector
    va_copy(vl2, vl);
    int log_size = std::vsnprintf(nullptr, 0, format, vl2);
    va_end(vl2);
    std::vector<char> log_buffer(log_size + 1);
    va_copy(vl2, vl);
    std::vsnprintf(log_buffer.data(), log_size, format, vl2);
    va_end(vl2);

    // Find out the component name
    const char* file = avc ? avc->item_name(ptr) : "ffmpeg";
    
    // Print using appropriate log level
    if (level <= AV_LOG_ERROR) LogTemporary(ERROR, file, 0) << log_buffer.data();
    else if (level <= AV_LOG_WARNING) LogTemporary(WARN, file, 0) << log_buffer.data();
    else if (level <= AV_LOG_INFO) LogTemporary(INFO, file, 0) << log_buffer.data();
    else if (level <= AV_LOG_DEBUG) LogTemporary(DEBUG, file, 0) << log_buffer.data();
    else LogTemporary(TRACE, file, 0) << log_buffer.data();
}
