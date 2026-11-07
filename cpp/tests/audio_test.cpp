#include "audio.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        // Format 0, 96 ticks per beat, 500000 us tempo, one A4 quarter note on a square program.
        const std::vector<std::uint8_t> midi{'M','T','h','d',0,0,0,6,0,0,0,1,0,96,'M','T','r','k',0,0,0,23,
            0x00,0xff,0x51,0x03,0x07,0xa1,0x20, 0x00,0xc0,16, 0x00,0x90,69,100, 0x60,0x80,69,0, 0x00,0xff,0x2f,0x00};
        const auto song = parse_midi(midi);
        check(song.division == 96 && song.length_ticks == 96 && song.events.size() == 4, "MIDI events and length");
        const std::vector<std::uint8_t> wav{'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,
            0x40,0x1f,0,0,0x40,0x1f,0,0,1,0,8,0,'d','a','t','a',4,0,0,0,128,255,128,0};
        const auto sound = parse_wav(wav);
        check(sound.rate == 8000 && sound.samples.size() == 4 && std::abs(sound.samples[1] - 127 / 128.0f) < 1e-6, "8-bit WAV");

        AudioMixer mixer;
        mixer.play_music(AudioMixer::jingle, std::make_shared<MidiSong>(song), false);
        std::vector<float> out(std::size_t(AudioMixer::kRate) * 2);
        mixer.render(out.data(), AudioMixer::kRate / 4);
        float peak = 0;
        for (int i = 0; i < AudioMixer::kRate / 2; ++i) peak = std::max(peak, std::abs(out[std::size_t(i)]));
        check(peak > 0.02f, "the note sounds");
        mixer.render(out.data(), AudioMixer::kRate);
        mixer.render(out.data(), AudioMixer::kRate);
        check(!mixer.music_playing(AudioMixer::jingle), "a jingle stops after its last note fades");
        mixer.set_enabled(false);
        mixer.play_sound(std::make_shared<WavSound>(sound));
        mixer.render(out.data(), 64);
        check(out[1] == 0.0f, "sound off mutes the mix");
        std::cout << "Audio checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
