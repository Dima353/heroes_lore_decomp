#pragma once
#include "platform.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if HEROES_LORE_LOW_POWER
using AudioReal = float;
#else
using AudioReal = double;
#endif

struct tsf; // TinySoundFont (third_party/tsf.h)

// ci / bw: the original plays MIDI and WAV through MMAPI players. The port mixes them itself:
// a small General-MIDI-style synthesizer and 8/16-bit PCM WAV playback, so every platform
// (Switch, Vita, 3DS, PC) behaves the same without external sound fonts.

struct MidiEvent { std::uint32_t tick{}; std::uint8_t status{}, data1{}, data2{}; std::uint32_t tempo{}; };
struct MidiSong {
    std::uint16_t division{96};
    std::vector<MidiEvent> events; // merged tracks; status 0xff marks a tempo change (tempo field)
    std::uint32_t length_ticks{};
};
MidiSong parse_midi(const std::vector<std::uint8_t>& bytes);

struct WavSound { int rate{8000}; std::vector<float> samples; };
WavSound parse_wav(const std::vector<std::uint8_t>& bytes);

class AudioMixer {
public:
    AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    ~AudioMixer();
    static constexpr int kRate = HEROES_LORE_LOW_POWER ? 22050 : 48000;
    enum Slot { map_music = 0, event_music = 1, jingle = 2 };

    // Music slots loop unless `loop` is false. Event music holds the map music while it plays.
    void play_music(Slot slot, std::shared_ptr<const MidiSong> song, bool loop);
    void stop_music(Slot slot);
    bool music_playing(Slot slot) const;
    void play_sound(std::shared_ptr<const WavSound> sound);
    void stop_all();
    void set_enabled(bool enabled);
    // Port option: play music through a General MIDI SoundFont (.sf2) instead of the built-in
    // synth. load_soundfont reads the whole file (about a second for 30 MB) and attaches it.
    bool load_soundfont(const std::string& path);
    bool load_soundfont_compressed(const void* data,std::size_t size);
    bool has_soundfont() const;
    // SoundFont voices per music slot (default 24 on PSP / 3DS, 64 elsewhere); Old 3DS uses fewer.
    void set_soundfont_voices(int voices) { soundfont_voices_ = voices; }
    void set_soundfont_enabled(bool enabled);
    // Interleaved stereo float frames; called from the audio callback (or tests).
    void render(float* out, int frames);

private:
    int soundfont_voices_{HEROES_LORE_LOW_POWER ? 24 : 64};
    struct Voice {
        bool active{}, releasing{}, drum{};
        int channel{}, note{}, program{};
        AudioReal phase{}, step{}, amplitude{}, level{}, decay{};
        AudioReal cutoff{1.0}, filtered{}, attack{}, attack_step{1.0};
        int wave{};
        std::uint32_t noise{0x12345u};
        int age{}, bend_seen{-1};
    };
    struct Channel { int program{}, volume{100}, expression{127}, bend{8192}, pan{64}; };
    struct Player {
        std::shared_ptr<const MidiSong> song;
        bool loop{}, playing{};
        std::size_t next{};
        AudioReal tick{}, ticks_per_sample{};
        std::array<Channel, 16> channels{};
        std::vector<Voice> voices;
        tsf* synth{}; // SoundFont copy for this slot when the option is on
    };
    struct SoundVoice { std::shared_ptr<const WavSound> sound; AudioReal position{}; float filtered{}; };
    struct DelayLine { std::vector<float> buffer; std::size_t position{}; float damp{}; };

    bool attach_soundfont(tsf* font);
    void start(Player& player);
    void render_soundfont(Player& player, float* out, int frames);
    void process(Player& player, const MidiEvent& event);
    void note_on(Player& player, int channel, int note, int velocity);
    float voice_sample(Player& player, Voice& voice);

    mutable std::mutex mutex_;
    std::array<Player, 3> players_{};
    std::vector<SoundVoice> sounds_;
    // Even lines feed the left side, odd lines the right (lengths in samples at 48 kHz).
    std::array<DelayLine, 4> reverb_{{{std::vector<float>(1687)}, {std::vector<float>(1777)},
                                      {std::vector<float>(2063)}, {std::vector<float>(1931)}}};
    bool enabled_{true};
    tsf* font_{};
    bool use_font_{true};
};
