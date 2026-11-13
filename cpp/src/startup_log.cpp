#include "startup_log.hpp"
#include "platform.hpp"
#include <SDL.h>
#include <cstdio>
#if !HEROES_LORE_CONSOLE
#include <iostream>
#endif

std::string startup_log_path;

void startup_log(const std::string& message) {
#if !HEROES_LORE_CONSOLE
    std::cerr << "heroes_lore: " << message << '\n';
#endif
    if (startup_log_path.empty()) return;
#if HEROES_LORE_3DS
    // Opening a file on the 3DS SD card costs ~85 ms: keep the log open, flush every line.
    static std::FILE* open_log = nullptr;
    if (!open_log) open_log = std::fopen(startup_log_path.c_str(), "a");
    if (open_log) {
        std::fprintf(open_log, "[%u] %s\n", unsigned(SDL_GetTicks()), message.c_str());
        std::fflush(open_log);
    }
    return;
#endif
    if (auto* log = std::fopen(startup_log_path.c_str(), "a")) {
        std::fputs(message.c_str(), log);
        std::fputc('\n', log);
        std::fflush(log);
        std::fclose(log);
    }
}

void reset_startup_log() {
    if (startup_log_path.empty()) return;
    if (auto* log = std::fopen(startup_log_path.c_str(), "w")) std::fclose(log);
}
