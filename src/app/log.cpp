#include "xpbd/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

#if __has_include(<spdlog/spdlog.h>)
#define XPBD_HAS_SPDLOG 1
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#else
#define XPBD_HAS_SPDLOG 0
#endif

namespace xpbd::log {
namespace {

#if XPBD_HAS_SPDLOG
std::shared_ptr<spdlog::logger> g_logger;
#else
std::mutex g_mu;
FILE* g_file = nullptr;
Level g_level = Level::Info;
#endif

}

void init(const std::string& path) {
#if XPBD_HAS_SPDLOG
    if (g_logger) {
        return;
    }
    try {

        spdlog::init_thread_pool(8192, 1);
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path, 2 * 1024 * 1024, 3);
        std::vector<spdlog::sink_ptr> sinks{console, file};
        g_logger = std::make_shared<spdlog::async_logger>(
            "xpbd", sinks.begin(), sinks.end(), spdlog::thread_pool(),
            spdlog::async_overflow_policy::overrun_oldest);
        g_logger->set_level(spdlog::level::info);
        g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        g_logger->flush_on(spdlog::level::warn);
        spdlog::register_logger(g_logger);
        spdlog::set_default_logger(g_logger);
        g_logger->info("logger ready ({})", path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "spdlog init failed: %s\n", e.what());
    }
#else
    std::lock_guard lock(g_mu);
    if (g_file) {
        return;
    }
    g_file = std::fopen(path.c_str(), "a");
    if (g_file) {
        std::setvbuf(g_file, nullptr, _IOFBF, 64 * 1024);
        std::fprintf(g_file, "[info] logger ready (fallback FILE*, no spdlog) %s\n", path.c_str());
        std::fflush(g_file);
    }
#endif
}

void shutdown() {
#if XPBD_HAS_SPDLOG
    if (g_logger) {
        g_logger->flush();
        spdlog::drop("xpbd");
        g_logger.reset();
        spdlog::shutdown();
    }
#else
    std::lock_guard lock(g_mu);
    if (g_file) {
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
#endif
}

void flush() {
#if XPBD_HAS_SPDLOG
    if (g_logger) {
        g_logger->flush();
    }
#else
    std::lock_guard lock(g_mu);
    if (g_file) {
        std::fflush(g_file);
    }
#endif
}

void setLevel(Level level) {
#if XPBD_HAS_SPDLOG
    if (!g_logger) {
        return;
    }
    switch (level) {
        case Level::Trace:
            g_logger->set_level(spdlog::level::trace);
            break;
        case Level::Debug:
            g_logger->set_level(spdlog::level::debug);
            break;
        case Level::Info:
            g_logger->set_level(spdlog::level::info);
            break;
        case Level::Warn:
            g_logger->set_level(spdlog::level::warn);
            break;
        case Level::Error:
            g_logger->set_level(spdlog::level::err);
            break;
    }
#else
    std::lock_guard lock(g_mu);
    g_level = level;
#endif
}

void log(Level level, std::string_view msg) {
#if XPBD_HAS_SPDLOG
    if (!g_logger) {
        init();
    }
    if (!g_logger) {
        return;
    }
    switch (level) {
        case Level::Trace:
            g_logger->trace("{}", msg);
            break;
        case Level::Debug:
            g_logger->debug("{}", msg);
            break;
        case Level::Info:
            g_logger->info("{}", msg);
            break;
        case Level::Warn:
            g_logger->warn("{}", msg);
            break;
        case Level::Error:
            g_logger->error("{}", msg);
            break;
    }
#else
    std::unique_lock lock(g_mu);
    if (static_cast<int>(level) < static_cast<int>(g_level)) {
        return;
    }
    if (!g_file) {
        lock.unlock();
        init();
        lock.lock();
    }
    if (!g_file) {
        return;
    }
    const char* tag = "info";
    switch (level) {
        case Level::Trace:
            tag = "trace";
            break;
        case Level::Debug:
            tag = "debug";
            break;
        case Level::Info:
            tag = "info";
            break;
        case Level::Warn:
            tag = "warn";
            break;
        case Level::Error:
            tag = "error";
            break;
    }
    std::fprintf(g_file, "[%s] %.*s\n", tag, static_cast<int>(msg.size()), msg.data());
#endif
}

void trace(std::string_view msg) { log(Level::Trace, msg); }
void debug(std::string_view msg) { log(Level::Debug, msg); }
void info(std::string_view msg) { log(Level::Info, msg); }
void warn(std::string_view msg) { log(Level::Warn, msg); }
void error(std::string_view msg) { log(Level::Error, msg); }

void infof(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    info(buf);
}

void warnf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    warn(buf);
}

void errorf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    error(buf);
}

}
