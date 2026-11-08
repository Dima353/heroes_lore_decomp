#pragma once
// Nintendo DS sound: bw (MIDI music, WAV effects) on the DS sound hardware.
// A calico thread on the ARM9 runs the MIDI sequencer at about 192 Hz and sends channel
// commands (IMA-ADPCM instruments from the built-in bank, PCM8 effects) to calico's ARM7
// sound driver; nothing is mixed in software.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace snd {
enum Slot { map_music = 0, event_music = 1, jingle = 2 };
using Loader = std::function<std::vector<std::uint8_t>(const std::string&)>;

// Reads snd/*.wav through `load` (kept for the MIDIs, read when a song starts) and starts the thread.
void init(Loader load, void (*log)(const std::string&));
void play_sound(int id);              // bw.a(id): a WAV effect, or a MIDI jingle once
void play_music(Slot slot, int id);   // bw.b / bw.c: looped; an id without a MIDI stops the slot
void stop_music(Slot slot);
bool music_playing(Slot slot);
void stop_all();                      // bw.f()
void set_enabled(bool enabled);       // Options Sound
void set_volume(int sound_level, int music_level, int effects_level); // DS Options: 0..10 each (Sound scales both)

struct Stats { unsigned bank_kb, sfx_kb, song_kb, voices, voices_peak, busy_us; };
Stats stats(); // busy_us: sound thread time over the last second
}
