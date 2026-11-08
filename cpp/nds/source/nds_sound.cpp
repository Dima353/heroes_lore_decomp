// Nintendo DS sound (see nds_sound.hpp). The MIDI files are parsed by the core's parse_midi
// (audio.cpp); instruments come from soundbank.bin, built from GeneralUser GS by
// tools/build_nds_soundbank.py and linked into the ARM9 binary.
#include "nds_sound.hpp"
#include "audio.hpp"
#include <nds.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include "soundbank_bin.h"

namespace snd {
namespace {
// bw: sound ids 0..31 -> /snd files ("def.mid" fills unused ids), as in main.cpp.
const char* const kSoundFiles[32] = {"00.mid", "01.mid", "02.mid", "03.mid", "04.mid", "05.mid", "06.mid", "07.mid",
    "08.wav", "def.mid", "def.mid", "def.mid", "12.mid", "13.wav", "14.wav", "15.wav", "16.wav", "17.wav", "18.wav", "def.mid",
    "20.wav", "21.wav", "22.mid", "23.mid", "24.mid", "25.mid", "26.mid", "27.mid", "28.mid", "29.mid", "30.mid", "31.mid"};
bool is_wav(int id) { return std::strstr(kSoundFiles[id], ".wav") != nullptr; }

// soundbank.bin records (build_nds_soundbank.py).
struct BankHeader { char magic[4]; u16 presets, zones, samples, pad; u32 data; };
struct BankPreset { u8 bank, program; u16 first, count, pad; };
struct BankZone { u8 lo, hi, root, pan; s16 tune; u16 sample, atten, attack, hold, decay, sustain, release, pad0, pad1; };
struct BankSample { u32 offset; u16 loop_words, pad; u32 words; u16 rate; u8 loop, pad2; };
static_assert(sizeof(BankZone) == 24 && sizeof(BankSample) == 16 && sizeof(BankPreset) == 8, "bank layout");

const BankHeader* bank_header = nullptr;
const BankPreset* bank_presets = nullptr;
const BankZone* bank_zones = nullptr;
const BankSample* bank_samples = nullptr;

constexpr int kMusicVoices = 13;            // channels 0..12; 13..15 play the WAV effects
constexpr int kFirstSfx = 13, kSfxChannels = 3;
constexpr int kMaxCb = 960;                 // envelope floor (96 dB): the voice is silent
constexpr int kSongMedianCb = 50;           // songs are levelled so their median note sits 5 dB below full scale
constexpr int kVolumeStepCb = 30;           // Options volume steps: 3 dB each below 10
constexpr unsigned kSfxVolume = 1500;       // of 2047
constexpr int kJingleCutCb = 60;            // MIDI jingles (game over, level up) 6 dB below the songs
constexpr u32 kWakeUs = 5208;               // ~192 Hz, calico's own driver rate

std::array<u16, kMaxCb + 1> cb_to_lin{};    // 2047 * 10^(-cB/200)
std::array<u16, 128> level_cb{};            // velocity / CC7 / CC11 -> cB (square law)
std::array<u32, 1200> pow2_q16{};           // 2^(c/1200) in Q16

struct Channel { u8 program{}, volume{100}, expression{127}, pan{64}; bool sustain{}; int bend_cents{}; };
struct Player {
    std::shared_ptr<const MidiSong> song;
    bool loop{}, playing{}, paused{}, cut{};
    int gain_cb{}; // song levelling (see song_gain)
    std::size_t next{};
    u64 pos_q24{}; // MIDI ticks in Q24
    u64 rate_q24{}; // MIDI ticks per calico tick in Q24
    std::array<Channel, 16> channels{};
};
enum Stage : u8 { attack, hold, decay, sustain, release };
struct Voice {
    bool on{}, pedal{};
    u8 slot{}, channel{}, note{}, velocity{};
    Stage stage{};
    const BankZone* zone{};
    const BankSample* sample{};
    u32 t_us{};          // time in the attack / hold stage
    int env_q8{};        // envelope attenuation in cB, Q8
    u32 release_ms{};
    u32 age{}, wakes{};
    u16 last_vol{0xffff}, last_timer{};
    u8 last_pan{};
};

Mutex lock;
Thread thread;
alignas(8) u8 thread_stack[6144]; // includes the C library's thread-local storage
std::array<Player, 3> players;
std::array<Voice, kMusicVoices> voices;
struct Effect { std::vector<u32> data; u32 words{}; u16 rate{8000}; };
std::array<Effect, 32> effects;
std::array<s8, 4> pending_sfx{-1, -1, -1, -1};
bool enabled = true, applied_enabled = true, kill_all = false;
int music_offset_cb = 0, sfx_offset_cb = 0; // Options Sound x Music / Sound x Effects (WAVs and MIDI jingles)
int sfx_next = 0;
u32 voice_age = 0;
unsigned voices_peak = 0, busy_us = 0, song_bytes = 0, sfx_bytes = 0;
u32 busy_ticks = 0;
Loader loader;
void (*log_line)(const std::string&) = nullptr;
std::array<std::shared_ptr<const MidiSong>, 32> song_cache;
std::array<u32, 32> song_used{};
std::array<int, 32> song_gain{};
u32 song_clock = 0;

void set_tempo(Player& p, u32 tempo_us) {
    if (!tempo_us) tempo_us = 500000;
    p.rate_q24 = (u64(p.song->division) * 1000000ull << 24) / (u64(tempo_us) * TICK_FREQ);
}
void reset_player(Player& p) {
    p.next = 0; p.pos_q24 = 0; p.channels = {}; p.paused = false;
    set_tempo(p, 500000); // 120 bpm until a tempo event
}

const BankZone* find_zone(int bank, int program, int note) {
    if (!bank_header) return nullptr;
    const BankPreset* preset = nullptr;
    for (int pass = 0; pass < 2 && !preset; ++pass) { // the program, else the bank's first (piano / standard kit)
        const int p = pass == 0 ? program : 0;
        for (u16 i = 0; i < bank_header->presets; ++i)
            if (bank_presets[i].bank == bank && bank_presets[i].program == p) { preset = &bank_presets[i]; break; }
    }
    if (!preset || !preset->count) return nullptr;
    const BankZone* best = nullptr; int best_d = 1 << 30;
    for (u16 i = 0; i < preset->count; ++i) {
        const BankZone& z = bank_zones[preset->first + i];
        if (note >= z.lo && note <= z.hi) return &z;
        const int d = note < z.lo ? z.lo - note : note - z.hi;
        if (d < best_d) { best_d = d; best = &z; }
    }
    return bank == 128 && best_d > 0 ? nullptr : best; // a drum key the songs never use
}

u16 voice_timer(const Voice& v) {
    const auto& ch = players[v.slot].channels[v.channel];
    int cents = (int(v.note) - int(v.zone->root)) * 100 + v.zone->tune + ch.bend_cents;
    int oct = cents >= 0 ? cents / 1200 : -((-cents + 1199) / 1200);
    const int frac = cents - oct * 1200;
    u64 t = ((u64(SOUND_CLOCK) << 16) / v.sample->rate << 16) / pow2_q16[std::size_t(frac)]; // Q16 timer
    if (oct > 0) t >>= oct; else t <<= -oct;
    return u16(std::clamp<u64>(t >> 16, 16, 65535));
}
u16 voice_volume(const Voice& v) {
    const auto& ch = players[v.slot].channels[v.channel];
    int cb = v.zone->atten + level_cb[v.velocity] + level_cb[ch.volume] + level_cb[ch.expression] - players[v.slot].gain_cb + (v.slot == jingle ? sfx_offset_cb : music_offset_cb) + (v.env_q8 >> 8);
    u32 lin = cb_to_lin[std::size_t(std::clamp(cb, 0, kMaxCb))];
    if (v.stage == attack && v.zone->attack) lin = lin * std::min<u32>(65536, (v.t_us << 6) / v.zone->attack) >> 16;
    return u16(lin);
}
u8 voice_pan(const Voice& v) {
    return u8(std::clamp(int(players[v.slot].channels[v.channel].pan) + int(v.zone->pan) - 64, 0, 127));
}

void stop_voice(int i) { voices[std::size_t(i)].on = false; soundStop(1u << i); }
void release_voice(Voice& v, u32 ms) {
    if (v.stage == attack && v.zone->attack) { // keep the current level: linear fraction -> cB
        const u32 frac = std::max<u32>(1, std::min<u32>(65536, (v.t_us << 6) / v.zone->attack));
        int cb = 0; while (cb < kMaxCb && (u32(cb_to_lin[std::size_t(cb)]) << 16) / 2047 > frac) cb += 4;
        v.env_q8 = cb << 8;
    }
    v.stage = release; v.release_ms = std::max<u32>(ms, 10); v.pedal = false;
}
void release_slot(int slot, u32 ms) {
    for (auto& v : voices) if (v.on && v.slot == slot && v.stage != release) release_voice(v, std::min<u32>(ms, v.zone->release));
}

void note_on(int slot, int ch, int note, int velocity) {
    if (!applied_enabled) return;
    const auto& c = players[std::size_t(slot)].channels[std::size_t(ch)];
    const BankZone* zone = find_zone(ch == 9 ? 128 : 0, c.program, note);
    if (!zone) return;
    // A free channel, else the quietest releasing voice, else the oldest.
    int pick = -1;
    for (int i = 0; i < kMusicVoices && pick < 0; ++i) if (!voices[std::size_t(i)].on) pick = i;
    if (pick < 0) {
        int loudest_q8 = -1; u32 oldest = ~0u;
        for (int i = 0; i < kMusicVoices; ++i) {
            const auto& v = voices[std::size_t(i)];
            if (v.stage == release && v.env_q8 > loudest_q8) { loudest_q8 = v.env_q8; pick = i; }
        }
        if (pick < 0) for (int i = 0; i < kMusicVoices; ++i) if (voices[std::size_t(i)].age < oldest) { oldest = voices[std::size_t(i)].age; pick = i; }
    }
    Voice& v = voices[std::size_t(pick)];
    v = Voice{};
    v.on = true; v.slot = u8(slot); v.channel = u8(ch); v.note = u8(note); v.velocity = u8(velocity);
    v.zone = zone; v.sample = &bank_samples[zone->sample];
    v.stage = zone->attack ? attack : zone->hold ? hold : decay;
    v.age = ++voice_age;
    v.last_vol = voice_volume(v); v.last_timer = voice_timer(v); v.last_pan = voice_pan(v);
    const auto* data = reinterpret_cast<const u8*>(bank_header) + v.sample->offset;
    const bool loop = v.sample->loop;
    soundPreparePcm(unsigned(pick) | SOUND_START, v.last_vol, v.last_pan, v.last_timer,
                    loop ? SoundMode_Repeat : SoundMode_OneShot, SoundFmt_ImaAdpcm, data,
                    loop ? v.sample->loop_words : 0, loop ? v.sample->words - v.sample->loop_words : v.sample->words);
    unsigned on = 0; for (const auto& x : voices) on += x.on;
    voices_peak = std::max(voices_peak, on);
}
void note_off(int slot, int ch, int note) {
    const bool pedal = players[std::size_t(slot)].channels[std::size_t(ch)].sustain;
    for (auto& v : voices)
        if (v.on && v.slot == slot && v.channel == ch && v.note == note && v.stage != release && !v.pedal) {
            if (pedal) v.pedal = true; else release_voice(v, v.zone->release);
        }
}
void refresh_channel(int slot, int ch, bool pitch) {
    for (std::size_t i = 0; i < voices.size(); ++i) {
        auto& v = voices[i];
        if (!v.on || v.slot != slot || v.channel != ch) continue;
        if (pitch) { const u16 t = voice_timer(v); if (t != v.last_timer) { v.last_timer = t; soundChSetTimer(unsigned(i), t); } }
        const u8 pan = voice_pan(v); if (pan != v.last_pan) { v.last_pan = pan; soundChSetPan(unsigned(i), pan); }
    }
}

void process(int slot, Player& p, const MidiEvent& e) {
    const int kind = e.status & 0xf0, ch = e.status & 15;
    auto& c = p.channels[std::size_t(ch)];
    if (e.status == 0xff) { set_tempo(p, e.tempo); return; }
    switch (kind) {
    case 0x90: if (e.data2) { note_on(slot, ch, e.data1 & 127, e.data2 & 127); break; } [[fallthrough]];
    case 0x80: note_off(slot, ch, e.data1 & 127); break;
    case 0xc0: c.program = e.data1 & 127; break;
    case 0xe0: c.bend_cents = ((int(e.data2) << 7 | e.data1) - 8192) * 200 / 8192; refresh_channel(slot, ch, true); break;
    case 0xb0:
        switch (e.data1) {
        case 7: c.volume = e.data2 & 127; break;
        case 10: c.pan = e.data2 & 127; refresh_channel(slot, ch, false); break;
        case 11: c.expression = e.data2 & 127; break;
        case 64:
            c.sustain = e.data2 >= 64;
            if (!c.sustain) for (auto& v : voices) if (v.on && v.pedal && v.slot == slot && v.channel == ch) release_voice(v, v.zone->release);
            break;
        case 121: c.volume = 100; c.expression = 127; c.pan = 64; c.bend_cents = 0; c.sustain = false; break;
        case 120: case 123:
            for (auto& v : voices) if (v.on && v.slot == slot && v.channel == ch && v.stage != release) release_voice(v, v.zone->release);
            break;
        }
        break;
    }
}

void advance(int slot, Player& p, u32 dt_ticks) {
    if (!p.playing || !p.song) return;
    const bool event_on = players[event_music].playing;
    if (slot == map_music && event_on) { // ah: event music holds the map music
        if (!p.paused) { p.paused = true; release_slot(slot, 60); }
        return;
    }
    p.paused = false;
    p.pos_q24 += u64(dt_ticks) * p.rate_q24;
    const auto& events = p.song->events;
    const u32 tick = u32(p.pos_q24 >> 24);
    while (p.next < events.size() && events[p.next].tick <= tick) process(slot, p, events[p.next++]);
    if (p.next >= events.size() && tick >= p.song->length_ticks) {
        release_slot(slot, 200);
        if (p.loop) { const u64 rate = p.rate_q24; reset_player(p); p.rate_q24 = rate; }
        else p.playing = false;
    }
}

void update_voices(u32 dt_us) {
    const unsigned active = soundGetActiveChannels();
    for (std::size_t i = 0; i < voices.size(); ++i) {
        auto& v = voices[i];
        if (!v.on) continue;
        if (++v.wakes > 3 && !(active & (1u << i))) { v.on = false; continue; } // a one-shot sample ended
        switch (v.stage) {
        case attack: v.t_us += dt_us; if (v.t_us >= u32(v.zone->attack) * 1000) { v.stage = v.zone->hold ? hold : decay; v.t_us = 0; } break;
        case hold: v.t_us += dt_us; if (v.t_us >= u32(v.zone->hold) * 1000) { v.stage = decay; v.t_us = 0; } break;
        case decay:
            if (!v.zone->decay) v.env_q8 = v.zone->sustain << 8;
            else v.env_q8 += int((256u * dt_us) / v.zone->decay);
            if (v.env_q8 >= (int(v.zone->sustain) << 8)) { v.env_q8 = v.zone->sustain << 8; v.stage = sustain; }
            break;
        case sustain: break;
        case release: v.env_q8 += int((256u * dt_us) / v.release_ms); break;
        }
        if ((v.env_q8 >> 8) >= kMaxCb) { stop_voice(int(i)); continue; }
        const u16 vol = voice_volume(v);
        if (vol != v.last_vol) { v.last_vol = vol; soundChSetVolume(unsigned(i), vol); }
    }
}

void start_effect(int id) {
    const auto& e = effects[std::size_t(id)];
    if (!e.words || !applied_enabled) return;
    const unsigned active = soundGetActiveChannels();
    int ch = -1;
    for (int k = 0; k < kSfxChannels && ch < 0; ++k) if (!(active & (1u << (kFirstSfx + k)))) ch = kFirstSfx + k;
    if (ch < 0) { ch = kFirstSfx + sfx_next; sfx_next = (sfx_next + 1) % kSfxChannels; }
    if (sfx_offset_cb >= kMaxCb) return;
    soundPreparePcm(unsigned(ch) | SOUND_START, kSfxVolume * cb_to_lin[std::size_t(sfx_offset_cb)] / 2047, 64, soundTimerFromHz(e.rate), SoundMode_OneShot, SoundFmt_Pcm8,
                    e.data.data(), 0, e.words);
}

int sound_main(void*) {
    u64 last = tickGetCount();
    u32 second = 0;
    for (;;) {
        threadSleep(kWakeUs);
        const u32 t0 = cpuGetTiming();
        const u64 now = tickGetCount();
        const u32 dt = u32(std::min<u64>(now - last, TICK_FREQ / 10)); // a stall never skips more than 0.1 s
        last = now;
        mutexLock(&lock);
        if (kill_all || enabled != applied_enabled) {
            applied_enabled = enabled; kill_all = false;
            for (auto& v : voices) v.on = false;
            soundStop(0xffff);
        }
        for (int s = 0; s < 3; ++s) if (players[std::size_t(s)].cut) { players[std::size_t(s)].cut = false; release_slot(s, 40); }
        for (auto& id : pending_sfx) if (id >= 0) { start_effect(id); id = -1; }
        for (int s = 0; s < 3; ++s) advance(s, players[std::size_t(s)], dt);
        update_voices(u32((u64(dt) * 1000000) / TICK_FREQ));
        mutexUnlock(&lock);
        busy_ticks += cpuGetTiming() - t0;
        if (++second >= 192) { busy_us = unsigned(u64(busy_ticks) * 1000000 / BUS_CLOCK); busy_ticks = 0; second = 0; }
    }
    return 0;
}

// The JAR's songs differ by up to 22 dB (median note: velocity, CC7, CC11); phone speakers hid that,
// the DS speakers do not. Boost quiet songs so the median note lands at kSongMedianCb.
int measure_gain(const MidiSong& song) {
    std::array<u8, 16> volume, expression; volume.fill(100); expression.fill(127);
    std::vector<u16> notes;
    for (const auto& e : song.events) {
        const int kind = e.status & 0xf0, ch = e.status & 15;
        if (kind == 0xb0 && e.data1 == 7) volume[std::size_t(ch)] = e.data2 & 127;
        else if (kind == 0xb0 && e.data1 == 11) expression[std::size_t(ch)] = e.data2 & 127;
        else if (kind == 0x90 && e.data2)
            notes.push_back(u16(level_cb[e.data2 & 127] + level_cb[volume[std::size_t(ch)]] + level_cb[expression[std::size_t(ch)]]));
    }
    if (notes.empty()) return 0;
    std::nth_element(notes.begin(), notes.begin() + notes.size() / 2, notes.end());
    return std::max(0, int(notes[notes.size() / 2]) - kSongMedianCb);
}

std::shared_ptr<const MidiSong> load_song(int id) {
    if (id < 0 || id >= 32 || is_wav(id) || !loader) return nullptr;
    song_used[std::size_t(id)] = ++song_clock;
    if (auto cached = song_cache[std::size_t(id)]) return cached;
    std::shared_ptr<const MidiSong> song;
    try { song = std::make_shared<MidiSong>(parse_midi(loader(std::string("snd/") + kSoundFiles[id]))); }
    catch (const std::exception& error) { if (log_line) log_line(std::string("sound ") + kSoundFiles[id] + " failed: " + error.what()); return nullptr; }
    // Keep at most four parsed songs (map, event, jingle and the last one).
    int cached = 0, oldest = -1;
    for (int i = 0; i < 32; ++i) if (song_cache[std::size_t(i)]) {
        ++cached;
        if (oldest < 0 || song_used[std::size_t(i)] < song_used[std::size_t(oldest)]) oldest = i;
    }
    if (cached >= 4 && oldest >= 0) song_cache[std::size_t(oldest)].reset();
    song_cache[std::size_t(id)] = song;
    song_gain[std::size_t(id)] = measure_gain(*song);
    song_bytes = 0;
    for (const auto& s : song_cache) if (s) song_bytes += unsigned(s->events.size() * sizeof(MidiEvent));
    return song;
}
} // namespace

void init(Loader load, void (*log)(const std::string&)) {
    loader = std::move(load); log_line = log;
    float lin = 2047.0f;
    for (auto& v : cb_to_lin) { v = u16(lin + 0.5f); lin *= 0.98855309f; } // 10^(-1/200) per cB
    float p = 65536.0f;
    for (auto& v : pow2_q16) { v = u32(p + 0.5f); p *= 1.00057779f; } // 2^(1/1200) per cent
    level_cb[0] = kMaxCb;
    for (int i = 1; i < 128; ++i) level_cb[std::size_t(i)] = u16(std::min(float(kMaxCb), -400.0f * std::log10(float(i) / 127.0f)));
    const auto* header = reinterpret_cast<const BankHeader*>(soundbank_bin);
    if (soundbank_bin_size >= sizeof(BankHeader) && std::memcmp(header->magic, "HLSB", 4) == 0 && header->presets) {
        bank_header = header;
        bank_presets = reinterpret_cast<const BankPreset*>(soundbank_bin + sizeof(BankHeader));
        bank_zones = reinterpret_cast<const BankZone*>(bank_presets + header->presets);
        bank_samples = reinterpret_cast<const BankSample*>(bank_zones + header->zones);
        DC_FlushRange(soundbank_bin, soundbank_bin_size);
    } else if (log_line) log_line("sound bank missing: music is silent");
    for (int id = 0; id < 32; ++id) {
        if (!is_wav(id)) continue;
        try { // 8-bit unsigned mono WAV -> signed PCM8 for the hardware
            const auto bytes = loader(std::string("snd/") + kSoundFiles[id]);
            const WavSound wav = parse_wav(bytes);
            auto& e = effects[std::size_t(id)];
            e.rate = u16(std::clamp(wav.rate, 1000, 48000));
            e.words = u32((wav.samples.size() + 3) / 4);
            e.data.assign(e.words, 0);
            auto* out = reinterpret_cast<s8*>(e.data.data());
            for (std::size_t i = 0; i < wav.samples.size(); ++i) out[i] = s8(std::clamp(int(wav.samples[i] * 127.0f), -128, 127));
            DC_FlushRange(e.data.data(), e.words * 4);
            sfx_bytes += e.words * 4;
        } catch (const std::exception& error) { if (log_line) log_line(std::string("sound ") + kSoundFiles[id] + " failed: " + error.what()); }
    }
    soundInit();
    soundPowerOn();
    soundSetAutoUpdate(true);
    soundSetMixerVolume(127);
    mutexLock(&lock); mutexUnlock(&lock);
    threadPrepare(&thread, sound_main, nullptr, &thread_stack[sizeof(thread_stack)], MAIN_THREAD_PRIO - 4);
    threadAttachLocalStorage(&thread, nullptr);
    threadStart(&thread);
    if (log_line) {
        char text[128];
        std::snprintf(text, sizeof(text), "sound: bank %u KB (%u presets, %u samples), effects %u KB",
                      unsigned(soundbank_bin_size / 1024), bank_header ? unsigned(bank_header->presets) : 0u,
                      bank_header ? unsigned(bank_header->samples) : 0u, sfx_bytes / 1024);
        log_line(text);
    }
}

void play_sound(int id) {
    if (id < 0 || id >= 32) return;
    if (!is_wav(id)) { // a MIDI jingle once
        auto song = load_song(id);
        if (!song) return;
        mutexLock(&lock);
        auto& p = players[jingle];
        p.cut = p.playing; std::swap(p.song, song); p.loop = false; reset_player(p); p.playing = true; p.gain_cb = song_gain[std::size_t(id)] - kJingleCutCb;
        mutexUnlock(&lock);
        return;
    }
    mutexLock(&lock);
    for (auto& slot : pending_sfx) if (slot < 0) { slot = s8(id); break; }
    mutexUnlock(&lock);
}
void play_music(Slot slot, int id) {
    auto song = load_song(id);
    if (!song) { stop_music(slot); return; }
    mutexLock(&lock);
    auto& p = players[slot];
    p.cut = p.playing; std::swap(p.song, song); p.loop = true; reset_player(p); p.playing = true; p.gain_cb = song_gain[std::size_t(id)];
    mutexUnlock(&lock);
}
void stop_music(Slot slot) {
    std::shared_ptr<const MidiSong> old;
    mutexLock(&lock);
    auto& p = players[slot];
    if (p.playing) p.cut = true;
    p.playing = false; std::swap(old, p.song);
    mutexUnlock(&lock);
}
bool music_playing(Slot slot) {
    mutexLock(&lock);
    const bool playing = players[slot].playing;
    mutexUnlock(&lock);
    return playing;
}
void stop_all() {
    std::array<std::shared_ptr<const MidiSong>, 3> old;
    mutexLock(&lock);
    for (std::size_t s = 0; s < 3; ++s) { players[s].playing = false; std::swap(old[s], players[s].song); }
    pending_sfx.fill(-1);
    kill_all = true;
    mutexUnlock(&lock);
}
void set_enabled(bool on) {
    if (on == enabled) return;
    mutexLock(&lock);
    enabled = on;
    mutexUnlock(&lock);
}
void set_volume(int sound_level, int music_level, int effects_level) {
    const auto offset = [](int level) { return level <= 0 ? kMaxCb : (10 - std::min(level, 10)) * kVolumeStepCb; };
    const int sfx = std::min(kMaxCb, offset(sound_level) + offset(effects_level)), music = std::min(kMaxCb, offset(sound_level) + offset(music_level));
    if (sfx == sfx_offset_cb && music == music_offset_cb) return;
    mutexLock(&lock);
    sfx_offset_cb = sfx; music_offset_cb = music; // voices pick the new level up on the next wake
    mutexUnlock(&lock);
}
Stats stats() {
    mutexLock(&lock);
    unsigned on = 0; for (const auto& v : voices) on += v.on;
    const Stats s{unsigned(soundbank_bin_size / 1024), sfx_bytes / 1024, song_bytes / 1024, on, voices_peak, busy_us};
    mutexUnlock(&lock);
    return s;
}
} // namespace snd
