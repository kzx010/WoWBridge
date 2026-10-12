// WoWBridge replacement for WoWee's core/logger.hpp: the same LOG_* macros, routed to one sink that the host sets
// (spdlog inside the SKSE plugin, stderr in native tests). Messages are built only when the level is enabled.
#pragma once
#include <functional>
#include <sstream>
#include <string>

namespace wowee::core {
enum class LogLevel { DEBUG, INFO, WARNING, ERROR, FATAL };
inline constexpr LogLevel kLogLevelError = LogLevel::ERROR;

struct LogSink {
    LogLevel min = LogLevel::WARNING;
    std::function<void(LogLevel, const std::string&)> write;
    static LogSink& get() { static LogSink s; return s; }
    bool enabled(LogLevel l) const { return l >= min; }
    template <typename... A> void log(LogLevel l, A&&... a) {
        std::ostringstream os;
        (os << ... << a);
        if (write) write(l, os.str());
    }
};
// WoWee's Logger facade, for the one call site that picks its level at run time.
struct Logger {
    static Logger& getInstance() { static Logger l; return l; }
    bool shouldLog(LogLevel l) const { return LogSink::get().enabled(l); }
    template <typename... A> void at(LogLevel l, A&&... a) {
        if (shouldLog(l)) LogSink::get().log(l, std::forward<A>(a)...);
    }
    template <typename... A> void debug(A&&... a) { at(LogLevel::DEBUG, std::forward<A>(a)...); }
    template <typename... A> void info(A&&... a) { at(LogLevel::INFO, std::forward<A>(a)...); }
    template <typename... A> void warning(A&&... a) { at(LogLevel::WARNING, std::forward<A>(a)...); }
    template <typename... A> void error(A&&... a) { at(LogLevel::ERROR, std::forward<A>(a)...); }
};
}  // namespace wowee::core

#define WOWEE_LOG_AT(level, ...) do { \
    auto& _wowee_sink = wowee::core::LogSink::get(); \
    if (_wowee_sink.enabled(level)) _wowee_sink.log(level, __VA_ARGS__); \
} while (0)
#define LOG_DEBUG(...)   WOWEE_LOG_AT(wowee::core::LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...)    WOWEE_LOG_AT(wowee::core::LogLevel::INFO, __VA_ARGS__)
#define LOG_WARNING(...) WOWEE_LOG_AT(wowee::core::LogLevel::WARNING, __VA_ARGS__)
#define LOG_ERROR(...)   WOWEE_LOG_AT(wowee::core::LogLevel::ERROR, __VA_ARGS__)
#define LOG_FATAL(...)   WOWEE_LOG_AT(wowee::core::LogLevel::FATAL, __VA_ARGS__)
