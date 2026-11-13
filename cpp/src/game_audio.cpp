#include "game_audio.hpp"
#include "audio.hpp"
#include <vector>
#if HEROES_LORE_PSP
#include "platform_psp.hpp"
#include "psp_diagnostics.hpp"
#endif

std::atomic<Uint32> audio_peak_ms{0};
void audio_callback(void* user,Uint8* stream,int length) {
#if HEROES_LORE_PSP
    psp_configure_fpu();
    psp_diag_audio_stage(1);
#endif
    const Uint32 started=SDL_GetTicks();
    const int frames=length/4; // S16 stereo
    // Plain static, not thread_local: a TLS destructor at SDL's audio-thread exit crashed libnx on quit.
    static std::vector<float> mix;
    const int synth_frames=HEROES_LORE_LOW_POWER?(frames+1)/2:frames;
    mix.resize(std::size_t(synth_frames)*2);
#if HEROES_LORE_PSP
    psp_diag_audio_stage(2);
#endif
    static_cast<AudioMixer*>(user)->render(mix.data(),synth_frames);
#if HEROES_LORE_PSP
    psp_diag_audio_stage(3);
#endif
    auto* out=reinterpret_cast<Sint16*>(stream);
    for(int i=0;i<frames*2;++i) out[i]=Sint16(mix[HEROES_LORE_LOW_POWER?std::size_t((i/4)*2+i%2):std::size_t(i)]*32767.0f);
    if(HEROES_LORE_PSP) {
#if HEROES_LORE_PSP
        psp_diag_audio_stage(0);
#endif
        const Uint32 elapsed=SDL_GetTicks()-started;
        Uint32 previous=audio_peak_ms.load();
        while(elapsed>previous && !audio_peak_ms.compare_exchange_weak(previous,elapsed)) {}
    }
}
