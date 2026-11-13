#pragma once
#include <SDL.h>
#include <atomic>
#include "platform.hpp"
#if HEROES_LORE_3DS
#include "platform_3ds_screen.hpp"
#endif

// bw: sound ids 0..31 -> /snd files ("def.mid" fills unused ids).
static const char* const kSoundFiles[32]={"00.mid","01.mid","02.mid","03.mid","04.mid","05.mid","06.mid","07.mid",
    "08.wav","def.mid","def.mid","def.mid","12.mid","13.wav","14.wav","15.wav","16.wav","17.wav","18.wav","def.mid",
    "20.wav","21.wav","22.mid","23.mid","24.mid","25.mid","26.mid","27.mid","28.mid","29.mid","30.mid","31.mid"};
// ae.h: field music per map tileset (-1 silent); maps 79..81 play 4.
static constexpr int kMapMusic[20]={28,0,27,29,-1,1,26,31,31,2,-1,25,24,24,30,3,-1,-1,-1,-1};
static constexpr const char* kEmbeddedSoundFont="<built-in SoundFont>";
extern std::atomic<Uint32> audio_peak_ms; // PSP: the slowest audio callback since the last perf line
// SDL audio callback: renders the AudioMixer passed as user data into S16 stereo.
void audio_callback(void* user,Uint8* stream,int length);
// Stops the device before the mixer it renders from goes away (also on exceptions).
// On Switch a thread that exits mid-process crashes in libnx (threadExit -> eh_globals_dtor),
// so SDL's audio thread is only paused there and dies with the process.
struct AudioDevice {
    SDL_AudioDeviceID id{};
    void stop() {
        if(!id) return;
#if HEROES_LORE_3DS
        // Close it: SDL then calls ndspExit. Left running, libctru's DSP thread outlived the heap at exit.
        aptUnhook(&g_3ds_apt_cookie);g_3ds_audio=0;
        SDL_CloseAudioDevice(id);
#elif HEROES_LORE_CONSOLE
        SDL_PauseAudioDevice(id,1); // returns once no callback is running
#else
        SDL_CloseAudioDevice(id);
#endif
        id=0;
    }
    ~AudioDevice() {stop();}
};
