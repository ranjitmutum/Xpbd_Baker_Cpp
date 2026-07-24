#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace xpbd::log {

enum class Level { Trace, Debug, Info, Warn, Error };


void init(const std::string& path = "xpbd_baker.log");
void shutdown();

void setLevel(Level level);

void log(Level level, std::string_view msg);

void trace(std::string_view msg);
void debug(std::string_view msg);
void info(std::string_view msg);
void warn(std::string_view msg);
void error(std::string_view msg);


void infof(const char* fmt, ...);
void warnf(const char* fmt, ...);
void errorf(const char* fmt, ...);

}
