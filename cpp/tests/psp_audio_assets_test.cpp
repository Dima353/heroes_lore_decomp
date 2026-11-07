#include "audio.hpp"
#include "third_party/tsf.h"
#include "frame_timing.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
static std::vector<unsigned char> bytes(const std::string& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main() {
    for(unsigned fps:{30u,60u}) for(unsigned period:{71u,100u,125u}) {
        unsigned previous=0,ticks=0;
        for(unsigned frame=1;frame<=fps*10;++frame) {unsigned now=frame*1000/fps;if(now-previous>=period){previous=advance_logic_tick(previous,now,period);++ticks;}}
        if(ticks!=10000/period) throw std::runtime_error("Logic rate changed with render FPS");
    }
    if(advance_logic_tick(100,1000,71)!=1000) throw std::runtime_error("Pause catchup was not bounded");
    auto* font=tsf_load_filename("builds/psp-audio-assets/heroes_lore.sf2");
    if(!font) throw std::runtime_error("Cannot load subset SF2");
    tsf_set_output(font,TSF_STEREO_INTERLEAVED,22050,0);tsf_set_max_voices(font,8);
    auto used=bytes("builds/psp-audio-assets/used-notes.bin");std::vector<float> note_buffer(4096);
    for(std::size_t i=0;i<used.size();i+=5) {
        tsf_reset(font);tsf_render_float(font,note_buffer.data(),2048,0);
        const int bank_id=used[i]|(used[i+1]<<8),program=used[i+2]|(used[i+3]<<8),note=used[i+4];
        if(!tsf_bank_note_on(font,bank_id,program,note,.8f) || !tsf_active_voice_count(font))
            throw std::runtime_error("Used MIDI instrument/note is missing from subset");
    }
    tsf_close(font);std::cout<<"All "<<used.size()/5<<" used MIDI bank/program/note combinations: PASS\n";
    auto bank=bytes("builds/psp-audio-assets/heroes_lore.sf2.zlib");
    AudioMixer mixer;if(!mixer.load_soundfont_compressed(bank.data(),bank.size())) throw std::runtime_error("Cannot load embedded SF2 stream");
    std::vector<float> out(2048);double total=0;
    for(const char* n:{"00.mid","01.mid","02.mid","03.mid","04.mid","05.mid","06.mid","07.mid","12.mid","22.mid","23.mid","24.mid","25.mid","26.mid","27.mid","28.mid","29.mid","30.mid","31.mid","def.mid"}) {
        auto song=std::make_shared<MidiSong>(parse_midi(bytes(std::string("builds/psp-audio-assets/snd/")+n)));
        mixer.play_music(AudioMixer::map_music,song,true);double energy=0;
        for(int block=0;block<110;++block) {
            mixer.render(out.data(),1024);
            for(float v:out) {if(!std::isfinite(v) || std::abs(v)>1.001f)throw std::runtime_error("Invalid SF2 output");energy+=std::abs(v);}
        }
        if(energy<.01)throw std::runtime_error(std::string("Silent MIDI: ")+n);
        total+=energy;std::cout<<n<<" PASS energy="<<energy<<'\n';
    }
    AudioMixer malformed;unsigned char bad[]{1,2,3};if(malformed.load_soundfont_compressed(bad,sizeof(bad)))throw std::runtime_error("Invalid bank accepted");
    bank.back()^=1;if(malformed.load_soundfont_compressed(bank.data(),bank.size()))throw std::runtime_error("Broken compressed checksum accepted");
    std::cout<<"PASS fixed tick timing at 30/60 FPS, 20 MIDI tracks, compressed SF2 integrity; energy="<<total<<'\n';
}
