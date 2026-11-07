#pragma once
#include <array>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <optional>

struct PortSettings {
    int game_speed{2};
    bool instant_speech{}, smooth_camera{true};
    int language{}; // 0: the JAR's own text, 1: Russian language pack (byte 9, formerly reserved 0)
    bool heroes_unlocked{}; // bs.b after the first ending (byte 10 bit 0, formerly reserved 0)
    bool sound{true};       // be: sound on/off (byte 10 bit 1 set = off)
    bool soundfont{true};   // port: music through a .sf2 when one is present (byte 10 bit 2 set = off)
    bool smooth_motion{true}; // port: 60 fps movement between logic ticks (byte 10 bit 3 set = off)
    bool wide_ui{true};       // port: menus widened for wide screens (byte 10 bit 4 set = Java layout)
    bool autosave{true};      // port: autosave.hls on every map entry (byte 10 bit 5 set = off)
    bool modern_ui{};         // port: Modern menu layout over Wide (byte 10 bit 6 set = Modern)
    bool rumble{true};        // port: pad rumble on damage and screen shake (byte 10 bit 7 set = off)
};
inline std::optional<PortSettings> read_port_settings_file(const std::string& path) {
    auto* file=std::fopen(path.c_str(),"rb");if(!file) return {};
    std::array<unsigned char,12> bytes{};
    const auto count=std::fread(bytes.data(),1,bytes.size(),file);
    const bool exact=std::fgetc(file)==EOF && !std::ferror(file);std::fclose(file);
    unsigned char sum=0;for(int i=0;i<11;++i) sum^=bytes[i];
    if(count==12 && exact && bytes[0]=='H' && bytes[1]=='L' && bytes[2]=='S' &&
       bytes[3]=='E' && bytes[4]=='T' && bytes[5]==1 && bytes[6]<=2 &&
       bytes[7]<=1 && bytes[8]<=1 && bytes[9]<=1 && bytes[11]==sum)
        return PortSettings{bytes[6],bool(bytes[7]),bool(bytes[8]),bytes[9],bool(bytes[10]&1),!(bytes[10]&2),!(bytes[10]&4),!(bytes[10]&8),!(bytes[10]&16),!(bytes[10]&32),(bytes[10]&64)!=0,!(bytes[10]&128)};
    return {};
}
inline PortSettings read_port_settings(const std::string& path) {
    if(const auto settings=read_port_settings_file(path)) return *settings;
    if(const auto settings=read_port_settings_file(path+".bak")) return *settings;
    return {};
}
inline void write_port_settings(const std::string& path,const PortSettings& settings) {
    if(settings.game_speed<0 || settings.game_speed>2) throw std::runtime_error("Invalid game speed");
    std::array<unsigned char,12> bytes{'H','L','S','E','T',1,
        static_cast<unsigned char>(settings.game_speed),static_cast<unsigned char>(settings.instant_speech),
        static_cast<unsigned char>(settings.smooth_camera),static_cast<unsigned char>(settings.language!=0),static_cast<unsigned char>((settings.heroes_unlocked?1:0)|(settings.sound?0:2)|(settings.soundfont?0:4)|(settings.smooth_motion?0:8)|(settings.wide_ui?0:16)|(settings.autosave?0:32)|(settings.modern_ui?64:0)|(settings.rumble?0:128)),0};
    for(int i=0;i<11;++i) bytes[11]^=bytes[i];
    namespace fs=std::filesystem;
    const auto parent=fs::path(path).parent_path();if(!parent.empty()) fs::create_directories(parent);
    const auto temporary=path+".tmp";auto* file=std::fopen(temporary.c_str(),"wb");
    if(!file) throw std::runtime_error("Cannot open temporary settings");
    const bool written=std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
    const bool flushed=std::fflush(file)==0;const bool closed=std::fclose(file)==0;
    if(!written || !flushed || !closed) throw std::runtime_error("Settings write failed");
    if(!read_port_settings_file(temporary)) throw std::runtime_error("Temporary settings verification failed");
    const bool previous=fs::exists(path);
    const auto backup=path+(read_port_settings_file(path)?".bak":".corrupt");
    if(previous) {if(fs::exists(backup)) fs::remove(backup);fs::rename(path,backup);}
    try {fs::rename(temporary,path);}
    catch(...) {if(previous) {std::error_code error;fs::rename(backup,path,error);}throw;}
}
