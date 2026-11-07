#include "audio.hpp"
#include <cmath>
#if HEROES_LORE_LOW_POWER
#define TSF_REAL float
#define TSF_RELATIVE_SAMPLE_POSITION
#define TSF_POW powf
#define TSF_POWF powf
#define TSF_EXPF expf
#define TSF_LOG logf
#define TSF_TAN tanf
#define TSF_LOG10 log10f
#define TSF_SQRT sqrtf
#define TSF_SQRTF sqrtf
#endif
#define TSF_IMPLEMENTATION
#include "third_party/tsf.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <zlib.h>
#include <limits>

namespace {
std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t p) {
    if (p + 4 > b.size()) throw std::runtime_error("truncated MIDI");
    return std::uint32_t(b[p]) << 24 | std::uint32_t(b[p + 1]) << 16 | std::uint32_t(b[p + 2]) << 8 | b[p + 3];
}
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t p) {
    if (p + 4 > b.size()) throw std::runtime_error("truncated WAV");
    return std::uint32_t(b[p]) | std::uint32_t(b[p + 1]) << 8 | std::uint32_t(b[p + 2]) << 16 | std::uint32_t(b[p + 3]) << 24;
}
std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t p) {
    if (p + 2 > b.size()) throw std::runtime_error("truncated WAV");
    return std::uint16_t(b[p] | b[p + 1] << 8);
}
constexpr AudioReal kPi = AudioReal(3.14159265358979323846);
}

MidiSong parse_midi(const std::vector<std::uint8_t>& b) {
    if (b.size() < 14 || b[0] != 'M' || b[1] != 'T' || b[2] != 'h' || b[3] != 'd') throw std::runtime_error("not a MIDI file");
    MidiSong song;
    const std::size_t header = 8 + be32(b, 4);
    const int tracks = b[10] << 8 | b[11];
    song.division = std::uint16_t(b[12] << 8 | b[13]);
    if (song.division == 0 || (song.division & 0x8000)) song.division = 96; // SMPTE timing is not used here
    std::size_t p = header;
    for (int t = 0; t < tracks && p + 8 <= b.size(); ++t) {
        const std::size_t length = be32(b, p + 4), end = std::min(b.size(), p + 8 + length);
        std::size_t q = p + 8;
        p = end;
        std::uint32_t tick = 0;
        std::uint8_t running = 0;
        const auto varlen = [&]() {
            std::uint32_t v = 0;
            for (int i = 0; i < 4 && q < end; ++i) { const auto c = b[q++]; v = v << 7 | (c & 0x7f); if (!(c & 0x80)) break; }
            return v;
        };
        while (q < end) {
            tick += varlen();
            if (q >= end) break;
            std::uint8_t status = b[q];
            if (status & 0x80) ++q; else status = running;
            if (status == 0xff) {
                if (q >= end) break;
                const auto type = b[q++];
                const auto size = varlen();
                if (type == 0x51 && size == 3 && q + 3 <= end)
                    song.events.push_back({tick, 0xff, 0, 0, std::uint32_t(b[q] << 16 | b[q + 1] << 8 | b[q + 2])});
                if (type == 0x2f) { q = end; break; }
                q += size;
            } else if (status == 0xf0 || status == 0xf7) {
                q += varlen();
            } else if (status >= 0x80) {
                running = status;
                const int kind = status & 0xf0;
                const std::uint8_t d1 = q < end ? b[q++] : 0;
                std::uint8_t d2 = 0;
                if (kind != 0xc0 && kind != 0xd0) d2 = q < end ? b[q++] : 0;
                song.events.push_back({tick, status, d1, d2, 0});
            } else break;
        }
        song.length_ticks = std::max(song.length_ticks, tick);
    }
    std::stable_sort(song.events.begin(), song.events.end(), [](const MidiEvent& a, const MidiEvent& c) { return a.tick < c.tick; });
    return song;
}

WavSound parse_wav(const std::vector<std::uint8_t>& b) {
    if (b.size() < 12 || b[0] != 'R' || b[1] != 'I' || b[2] != 'F' || b[3] != 'F' || b[8] != 'W') throw std::runtime_error("not a WAV file");
    WavSound sound;
    int channels = 1, bits = 8;
    for (std::size_t p = 12; p + 8 <= b.size();) {
        const auto size = le32(b, p + 4);
        const std::size_t body = p + 8, end = std::min<std::size_t>(b.size(), body + size);
        if (b[p] == 'f' && b[p + 1] == 'm' && b[p + 2] == 't') {
            channels = std::max(1, int(le16(b, body + 2)));
            sound.rate = int(le32(b, body + 4));
            bits = le16(b, body + 14);
        } else if (b[p] == 'd' && b[p + 1] == 'a' && b[p + 2] == 't' && b[p + 3] == 'a') {
            const int step = bits / 8 * channels;
            for (std::size_t q = body; q + std::size_t(step) <= end; q += std::size_t(step))
                sound.samples.push_back(bits == 8 ? (int(b[q]) - 128) / 128.0f : float(std::int16_t(le16(b, q))) / 32768.0f);
        }
        p = body + size + (size & 1);
    }
    if (sound.rate <= 0) sound.rate = 8000;
    return sound;
}

AudioMixer::AudioMixer() {
    if(HEROES_LORE_LOW_POWER) {
        // Allocate bounded voice pools on the main thread, before audio starts.
        for(auto& player:players_) player.voices.reserve(12);
        sounds_.reserve(8);
    }
}
AudioMixer::~AudioMixer() {
    for (auto& player : players_) if (player.synth) tsf_close(player.synth);
    if (font_) tsf_close(font_);
}
bool AudioMixer::load_soundfont(const std::string& path) {
    return attach_soundfont(tsf_load_filename(path.c_str()));
}
namespace {
struct FontInflater {
    z_stream state{};
    bool failed{},finished{};
    static int read(void* user,void* data,unsigned int count) {
        auto& self=*static_cast<FontInflater*>(user);
        self.state.next_out=static_cast<Bytef*>(data);self.state.avail_out=count;
        while(self.state.avail_out && !self.finished && !self.failed) {
            const auto before=self.state.total_out;
            const int result=inflate(&self.state,Z_NO_FLUSH);
            if(result==Z_STREAM_END) self.finished=true;
            else if(result!=Z_OK || before==self.state.total_out) self.failed=true;
        }
        return int(count-self.state.avail_out);
    }
    static int skip(void* user,unsigned int count) {
        unsigned char scratch[4096];
        while(count) {const auto n=std::min(count,unsigned(sizeof(scratch)));if(read(user,scratch,n)!=int(n)) return 0;count-=n;}
        return 1;
    }
};
}
bool AudioMixer::load_soundfont_compressed(const void* data,std::size_t size) {
    if(!data || size>std::numeric_limits<uInt>::max()) return false;
    FontInflater inflater;
    inflater.state.next_in=const_cast<Bytef*>(static_cast<const Bytef*>(data));inflater.state.avail_in=uInt(size);
    if(inflateInit(&inflater.state)!=Z_OK) return false;
    tsf_stream stream{&inflater,FontInflater::read,FontInflater::skip};
    tsf* font=tsf_load(&stream);
    unsigned char scratch[4096];
    while(!inflater.finished && !inflater.failed) FontInflater::read(&inflater,scratch,sizeof(scratch));
    const bool ok=font && !inflater.failed && inflater.finished;
    inflateEnd(&inflater.state);
    if(!ok) {if(font) tsf_close(font);return false;}
    return attach_soundfont(font);
}
bool AudioMixer::attach_soundfont(tsf* font) {
    if (!font) return false;
    tsf_set_output(font, TSF_STEREO_INTERLEAVED, kRate, -3.0f);
    std::lock_guard<std::mutex> lock(mutex_);
    if (font_) { tsf_close(font); return true; }
    font_ = font;
    for (auto& player : players_) if (player.playing && use_font_) start(player); // switch the running song over
    return true;
}
bool AudioMixer::has_soundfont() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return font_ != nullptr;
}
void AudioMixer::set_soundfont_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (use_font_ == enabled) return;
    use_font_ = enabled;
    for (auto& player : players_) if (player.playing) start(player);
}
void AudioMixer::start(Player& player) {
    if (player.synth && (!HEROES_LORE_LOW_POWER || !(font_ && use_font_))) {tsf_close(player.synth);player.synth=nullptr;}
    if (font_ && use_font_) {
        if(!player.synth) {
            player.synth = tsf_copy(font_);
            if(!player.synth) throw std::runtime_error("SoundFont voice allocation failed");
            if(!tsf_set_max_voices(player.synth, soundfont_voices_)) throw std::runtime_error("SoundFont voice pool allocation failed");
        } else tsf_reset(player.synth);
        // Create all 16 channels up front (tsf allocates channels lazily); channel 10 is drums.
        for (int ch = 0; ch < 16; ++ch) tsf_channel_set_presetnumber(player.synth, ch, 0, ch == 9);
    }
    player.next = 0;
    player.tick = 0;
    player.channels = {};
    player.voices.clear();
    player.ticks_per_sample = player.song->division * AudioReal(1e6) / AudioReal(500000.0) / kRate; // 120 bpm until a tempo event
    player.playing = true;
}
void AudioMixer::play_music(Slot slot, std::shared_ptr<const MidiSong> song, bool loop) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& player = players_[std::size_t(slot)];
    player.song = std::move(song);
    player.loop = loop;
    if (player.song) start(player); else player.playing = false;
}
void AudioMixer::stop_music(Slot slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (players_[std::size_t(slot)].synth) tsf_reset(players_[std::size_t(slot)].synth);
    players_[std::size_t(slot)].playing = false;
    players_[std::size_t(slot)].voices.clear();
}
bool AudioMixer::music_playing(Slot slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return players_[std::size_t(slot)].playing;
}
void AudioMixer::play_sound(std::shared_ptr<const WavSound> sound) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sound && sounds_.size() < 8) sounds_.push_back({std::move(sound), AudioReal(0.0)});
}
void AudioMixer::stop_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& player : players_) { player.playing = false; player.voices.clear(); if (player.synth) tsf_reset(player.synth); }
    sounds_.clear();
}
void AudioMixer::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
}

namespace {
constexpr AudioReal kSecond = AudioMixer::kRate;
AudioReal per_sample(AudioReal seconds) { return seconds > 0 ? std::exp(-AudioReal(1.0) / (seconds * kSecond)) : AudioReal(0.0); }
AudioReal lowpass_alpha(AudioReal hz) { return AudioReal(1.0) - std::exp(-AudioReal(2.0) * kPi * hz / kSecond); }
// Band-limited step correction (polyBLEP): removes the aliasing "ring" of hard edges.
AudioReal blep(AudioReal t, AudioReal dt) {
    if (t < dt) { t /= dt; return t + t - t * t - AudioReal(1.0); }
    if (t > AudioReal(1.0) - dt) { t = (t - AudioReal(1.0)) / dt; return t * t + t + t + AudioReal(1.0); }
    return AudioReal(0.0);
}
const std::array<float, 4097>& sine_table() {
    static const auto table = [] {
        std::array<float, 4097> t{};
        for (std::size_t i = 0; i < t.size(); ++i) t[i] = float(std::sin(2 * kPi * AudioReal(i) / AudioReal(4096.0)));
        return t;
    }();
    return table;
}
AudioReal sine(AudioReal phase) { // phase in turns
    phase -= std::floor(phase);
    const AudioReal x = phase * AudioReal(4096.0);
    const auto i = std::size_t(x);
    const auto& t = sine_table();
    return t[i] + (t[i + 1] - t[i]) * (x - AudioReal(i));
}
// General MIDI families (program / 8) -> timbre, brightness and envelope.
struct Patch { int wave; AudioReal cutoff, attack, decay; };
// wave: 0 piano, 1 bell, 2 organ, 3 square, 4 bass, 5 saw, 6 flute
constexpr Patch kPatches[16] = {
    {0, 3200, AudioReal(0.004), AudioReal(1.2)}, {1, 4500, AudioReal(0.002), AudioReal(0.8)}, {2, 2600, AudioReal(0.008), AudioReal(0.0)}, {3, 2200, AudioReal(0.004), AudioReal(1.0)},
    {4, 1100, AudioReal(0.004), AudioReal(0.0)}, {5, 2600, AudioReal(0.030), AudioReal(0.0)}, {5, 2400, AudioReal(0.040), AudioReal(0.0)}, {5, 3000, AudioReal(0.015), AudioReal(0.0)},
    {3, 2600, AudioReal(0.010), AudioReal(0.0)}, {6, 3000, AudioReal(0.012), AudioReal(0.0)}, {3, 3200, AudioReal(0.004), AudioReal(0.0)}, {5, 1800, AudioReal(0.080), AudioReal(0.0)},
    {5, 2400, AudioReal(0.010), AudioReal(0.0)}, {1, 3500, AudioReal(0.002), AudioReal(0.9)}, {1, 3000, AudioReal(0.002), AudioReal(0.6)}, {3, 2500, AudioReal(0.004), AudioReal(0.3)}};
}

void AudioMixer::note_on(Player& player, int channel, int note, int velocity) {
    Voice voice;
    voice.active = true;
    voice.channel = channel;
    voice.note = note;
    voice.drum = channel == 9;
    voice.program = player.channels[std::size_t(channel)].program;
    voice.amplitude = velocity / AudioReal(127.0);
    voice.level = AudioReal(1.0);
    if (voice.drum) {
        const bool kick = note == 35 || note == 36, hat = note == 42 || note == 44 || note == 46;
        voice.decay = per_sample(kick ? AudioReal(0.10) : hat ? AudioReal(0.025) : note == 49 || note == 57 ? AudioReal(0.35) : AudioReal(0.07));
        voice.cutoff = lowpass_alpha(kick ? 400 : hat ? 9000 : 5000);
        voice.step = kick ? AudioReal(90.0) / kRate : AudioReal(0.0);
    } else {
        const auto& patch = kPatches[std::size_t(voice.program / 8) & 15];
        voice.decay = patch.decay > 0 ? per_sample(patch.decay) : AudioReal(1.0);
        voice.cutoff = lowpass_alpha(std::min(patch.cutoff, AudioReal(1800.0) + AudioReal(440.0) * std::pow(AudioReal(2.0), (note - 69) / AudioReal(12.0)) * 4));
        voice.attack_step = AudioReal(1.0) / std::max(AudioReal(1.0), patch.attack * kRate);
        voice.wave = patch.wave;
    }
    if (player.voices.size() >= (HEROES_LORE_LOW_POWER ? 12u : 24u)) player.voices.erase(player.voices.begin());
    player.voices.push_back(voice);
}
void AudioMixer::process(Player& player, const MidiEvent& e) {
    if (e.status == 0xff) { player.ticks_per_sample = player.song->division * AudioReal(1e6) / std::max<std::uint32_t>(1, e.tempo) / kRate; return; }
    const int channel = e.status & 0x0f, kind = e.status & 0xf0;
    if (tsf* f = player.synth) {
        if (kind == 0x90 && e.data2 > 0) tsf_channel_note_on(f, channel, e.data1, e.data2 / 127.0f);
        else if (kind == 0x80 || kind == 0x90) tsf_channel_note_off(f, channel, e.data1);
        else if (kind == 0xc0) tsf_channel_set_presetnumber(f, channel, e.data1, channel == 9);
        else if (kind == 0xb0) tsf_channel_midi_control(f, channel, e.data1, e.data2);
        else if (kind == 0xe0) tsf_channel_set_pitchwheel(f, channel, e.data1 | e.data2 << 7);
        return;
    }
    auto& ch = player.channels[std::size_t(channel)];
    if (kind == 0x90 && e.data2 > 0) note_on(player, channel, e.data1, e.data2);
    else if (kind == 0x80 || kind == 0x90) {
        for (auto& v : player.voices) if (v.channel == channel && v.note == e.data1 && !v.releasing) v.releasing = true;
    } else if (kind == 0xc0) ch.program = e.data1;
    else if (kind == 0xb0) {
        if (e.data1 == 7) ch.volume = e.data2;
        else if (e.data1 == 10) ch.pan = e.data2;
        else if (e.data1 == 11) ch.expression = e.data2;
        else if (e.data1 == 120 || e.data1 == 123) for (auto& v : player.voices) if (v.channel == channel) v.releasing = true;
    } else if (kind == 0xe0) ch.bend = e.data1 | e.data2 << 7;
}
float AudioMixer::voice_sample(Player& player, Voice& v) {
    const auto& ch = player.channels[std::size_t(v.channel)];
    const AudioReal gain = v.amplitude * v.level * (ch.volume / AudioReal(127.0)) * (ch.expression / AudioReal(127.0)) * AudioReal(0.30);
    static const AudioReal release = per_sample(AudioReal(0.12));
    AudioReal sample = AudioReal(0.0);
    if (v.drum) {
        if (v.note == 35 || v.note == 36) { // kick: sine falling in pitch
            v.phase += v.step * (AudioReal(0.35) + AudioReal(0.65) * v.level);
            sample = sine(v.phase) * AudioReal(1.5);
        } else {
            v.noise ^= v.noise << 13; v.noise ^= v.noise >> 17; v.noise ^= v.noise << 5;
            sample = (AudioReal(v.noise & 0xffff) / AudioReal(32768.0) - AudioReal(1.0)) * AudioReal(0.6);
        }
        v.level *= v.decay;
    } else {
        if (v.bend_seen != ch.bend) { // pitch only changes with the note or a bend
            v.bend_seen = ch.bend;
            v.step = AudioReal(440.0) * std::pow(AudioReal(2.0), (v.note - 69 + (ch.bend - 8192) / AudioReal(8192.0) * AudioReal(2.0)) / AudioReal(12.0)) / kRate;
        }
        v.phase += v.step;
        v.phase -= std::floor(v.phase);
        const AudioReal t = v.phase, dt = v.step;
        const auto square = [&](AudioReal duty) {
            AudioReal s = t < duty ? AudioReal(1.0) : -AudioReal(1.0);
            s += blep(t, dt);
            AudioReal u = t + AudioReal(1.0) - duty; u -= std::floor(u);
            return s - blep(u, dt);
        };
        switch (v.wave) {
            case 0: sample = sine(t) * AudioReal(0.75) + sine(2 * t) * AudioReal(0.2) + (4 * std::abs(t - AudioReal(0.5)) - 1) * AudioReal(0.25); break;
            case 1: sample = sine(t) * AudioReal(0.8) + sine(AudioReal(3.01) * t) * AudioReal(0.25) * v.level; break;
            case 2: sample = sine(t) * AudioReal(0.6) + sine(2 * t) * AudioReal(0.3) + sine(3 * t) * AudioReal(0.15); break;
            case 3: sample = square(AudioReal(0.5)) * AudioReal(0.45); break;
            case 4: sample = (4 * std::abs(t - AudioReal(0.5)) - 1) * AudioReal(0.8) + square(AudioReal(0.5)) * AudioReal(0.15); break;
            case 5: sample = (2 * t - 1 - blep(t, dt)) * AudioReal(0.45); break;
            default: sample = sine(t) * AudioReal(0.8) + (4 * std::abs(t - AudioReal(0.5)) - 1) * AudioReal(0.15); break;
        }
        if (v.attack < AudioReal(1.0)) { v.attack = std::min(AudioReal(1.0), v.attack + v.attack_step); sample *= v.attack; }
        v.level *= v.releasing ? release : v.decay;
    }
    v.filtered += (sample - v.filtered) * v.cutoff; // one-pole low-pass per voice
    if (v.level < AudioReal(0.001)) v.active = false;
    return float(v.filtered * gain);
}
// SoundFont slots: render in blocks that end at the next MIDI event.
void AudioMixer::render_soundfont(Player& player, float* out, int frames) {
    const auto& events = player.song->events;
    for (int done = 0; done < frames;) {
        while (player.next < events.size() && events[player.next].tick <= player.tick) process(player, events[player.next++]);
        if (player.next >= events.size() && player.tick >= player.song->length_ticks) {
            if (!player.loop) {
                if (!tsf_active_voice_count(player.synth)) { player.playing = false; return; }
            } else { const AudioReal tempo = player.ticks_per_sample; start(player); player.ticks_per_sample = tempo; continue; }
        }
        int block = std::min(frames - done, 64);
        if (player.next < events.size()) {
            const AudioReal until = (events[player.next].tick - player.tick) / std::max(AudioReal(1e-9), player.ticks_per_sample);
            block = std::max(1, std::min(block, int(std::ceil(until))));
        }
        tsf_render_float(player.synth, out + std::size_t(done) * 2, block, 1);
        player.tick += block * player.ticks_per_sample;
        done += block;
    }
}
void AudioMixer::render(float* out, int frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(out, out + std::size_t(frames) * 2, 0.0f);
    if(HEROES_LORE_LOW_POWER && !enabled_) return;
    const bool event_on = players_[event_music].playing;
    for (std::size_t slot = 0; slot < players_.size(); ++slot) {
        auto& player = players_[slot];
        if (!player.playing || !player.song || (slot == map_music && event_on)) continue;
        if (player.synth) { render_soundfont(player, out, frames); continue; }
        const auto& events = player.song->events;
        for (int i = 0; i < frames; ++i) {
            while (player.next < events.size() && events[player.next].tick <= player.tick) process(player, events[player.next++]);
            if (player.next >= events.size() && player.tick >= player.song->length_ticks) {
                if (!player.loop) { if (player.voices.empty()) { player.playing = false; break; } }
                else { const AudioReal tempo = player.ticks_per_sample; start(player); player.ticks_per_sample = tempo; }
            }
            float left = 0.0f, right = 0.0f;
            for (auto& v : player.voices) {
                const float s = voice_sample(player, v);
                const float pan = player.channels[std::size_t(v.channel)].pan / 127.0f; // CC10, equal-ish power
                left += s * (1.15f - 0.6f * pan);
                right += s * (0.55f + 0.6f * pan);
            }
            player.voices.erase(std::remove_if(player.voices.begin(), player.voices.end(), [](const Voice& v) { return !v.active; }), player.voices.end());
            out[2 * i] += left;
            out[2 * i + 1] += right;
            player.tick += player.ticks_per_sample;
        }
    }
    // Light room reverb on the music: two damped feedback delays per side.
    if(!HEROES_LORE_LOW_POWER) for (int i = 0; i < frames; ++i) {
        const float dry = (out[2 * i] + out[2 * i + 1]) * 0.5f;
        float wet[2] = {0.0f, 0.0f};
        for (std::size_t c = 0; c < reverb_.size(); ++c) {
            auto& line = reverb_[c];
            const float delayed = line.buffer[line.position];
            line.damp += (delayed - line.damp) * 0.35f;
            line.buffer[line.position] = dry + line.damp * 0.62f;
            line.position = (line.position + 1) % line.buffer.size();
            wet[c & 1] += delayed;
        }
        out[2 * i] += wet[0] * 0.09f;
        out[2 * i + 1] += wet[1] * 0.09f;
    }
    for (auto& voice : sounds_) {
        const auto& s = *voice.sound;
        const AudioReal step = AudioReal(s.rate) / kRate;
        for (int i = 0; i < frames && voice.position + 1 < AudioReal(s.samples.size()); ++i) {
            const auto at = std::size_t(voice.position);
            const AudioReal f = voice.position - AudioReal(at);
            voice.filtered += float((s.samples[at] * (1 - f) + s.samples[at + 1] * f) - voice.filtered) * 0.45f; // soften 8 kHz steps
            out[2 * i] += voice.filtered * 0.6f;
            out[2 * i + 1] += voice.filtered * 0.6f;
            voice.position += step;
        }
    }
    sounds_.erase(std::remove_if(sounds_.begin(), sounds_.end(), [](const SoundVoice& v) {
        return v.position + 1 >= AudioReal(v.sound->samples.size()); }), sounds_.end());
    const float master = enabled_ ? 1.0f : 0.0f;
    for (int i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i] * master, -1.0f, 1.0f);
}
