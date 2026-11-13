#pragma once
#include <string>

// Startup log: stderr on desktop builds, plus the file startup_log_path names when it is set.
extern std::string startup_log_path;
void startup_log(const std::string& message);
void reset_startup_log();
