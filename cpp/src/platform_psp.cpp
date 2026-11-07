#include <pspkernel.h>
#include <pspctrl.h>
#include <pspsdk.h>
#include <pspdisplay.h>
#include "platform_psp.hpp"
#include "psp_diagnostics.hpp"
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <exception>
#include <malloc.h>
PSP_HEAP_SIZE_KB(-2048);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
namespace {
unsigned held_buttons;
struct Binding {unsigned mask; SDL_GameControllerButton button;};
constexpr Binding bindings[]{
 {PSP_CTRL_CROSS,SDL_CONTROLLER_BUTTON_A},{PSP_CTRL_CIRCLE,SDL_CONTROLLER_BUTTON_B},
 {PSP_CTRL_SQUARE,SDL_CONTROLLER_BUTTON_X},{PSP_CTRL_TRIANGLE,SDL_CONTROLLER_BUTTON_Y},
 {PSP_CTRL_LTRIGGER,SDL_CONTROLLER_BUTTON_LEFTSHOULDER},{PSP_CTRL_RTRIGGER,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
 {PSP_CTRL_START,SDL_CONTROLLER_BUTTON_START},{PSP_CTRL_SELECT,SDL_CONTROLLER_BUTTON_BACK},
 {PSP_CTRL_UP,SDL_CONTROLLER_BUTTON_DPAD_UP},{PSP_CTRL_DOWN,SDL_CONTROLLER_BUTTON_DPAD_DOWN},
 {PSP_CTRL_LEFT,SDL_CONTROLLER_BUTTON_DPAD_LEFT},{PSP_CTRL_RIGHT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT}};
}
void psp_poll_controls() {
    static bool initialized=false;
    if(!initialized) {sceCtrlSetSamplingCycle(0);sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);initialized=true;}
    SceCtrlData pad{};
    if(sceCtrlPeekBufferPositive(&pad,1)<=0) return;
    unsigned buttons=pad.Buttons;
    if(pad.Lx<64) buttons|=PSP_CTRL_LEFT;
    if(pad.Lx>192) buttons|=PSP_CTRL_RIGHT;
    if(pad.Ly<64) buttons|=PSP_CTRL_UP;
    if(pad.Ly>192) buttons|=PSP_CTRL_DOWN;
    const unsigned changed=held_buttons^buttons;held_buttons=buttons;
    for(const auto& binding:bindings) if(changed&binding.mask) {
        SDL_Event event{};event.type=(buttons&binding.mask)?SDL_CONTROLLERBUTTONDOWN:SDL_CONTROLLERBUTTONUP;
        event.cbutton.which=-4242;event.cbutton.button=binding.button;
        event.cbutton.state=(buttons&binding.mask)?SDL_PRESSED:SDL_RELEASED;
        SDL_PushEvent(&event);
    }
}
bool psp_button_held(SDL_GameControllerButton button) {
    for(const auto& binding:bindings) if(binding.button==button) return (held_buttons&binding.mask)!=0;
    return false;
}

unsigned psp_read_fpu_control() {
    unsigned control;
    __asm__ volatile("cfc1 %0, $31" : "=r"(control));
    return control;
}
unsigned psp_configure_fpu() {
    const unsigned previous=psp_read_fpu_control();
    // PSP FPU state belongs to the current thread. Apply on the main thread
    // and in SDL's audio callback rather than assuming thread inheritance.
    pspSdkDisableFPUExceptions();
    return previous;
}

namespace {
std::atomic<unsigned> diag_sequence{0}, diag_audio{0};
bool diag_enabled=false;
}
void psp_diag_audio_stage(unsigned stage) {diag_audio.store(stage,std::memory_order_relaxed);}
void psp_diag_stage(const char* stage,int first,int second) {
    if(!diag_enabled) return;
    const unsigned sequence=diag_sequence.fetch_add(1,std::memory_order_relaxed);
    if(sequence>=512) return;
    char line[224];
    const auto heap=mallinfo();
    const int length=std::snprintf(line,sizeof(line),
        "%u us=%u stage=%s a=%d b=%d audio=%u heap_used=%d heap_free=%d fcr=%08x\n",
        sequence,sceKernelGetSystemTimeLow(),stage,first,second,
        diag_audio.load(std::memory_order_relaxed),heap.uordblks,heap.fordblks,psp_read_fpu_control());
    const int file=sceIoOpen("./psp_trace.log",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND,0777);
    if(file>=0) {sceIoWrite(file,line,length>0?std::min(length,int(sizeof(line)-1)):0);sceIoClose(file);}
}
void psp_diag_reset() {
    const int flag=sceIoOpen("./trace.txt",PSP_O_RDONLY,0);
    diag_enabled=flag>=0;
    if(flag>=0) sceIoClose(flag);
    if(diag_enabled) sceIoRemove("./psp_trace.log");
    diag_sequence.store(0);
    psp_diag_stage("launch-v10");
    std::set_terminate([] {psp_diag_stage("C++-terminate");sceKernelExitGame();for(;;) {}});
}

void psp_pace_frame_30fps() {
    static unsigned deadline=0;
    unsigned now=sceDisplayGetVcount();
    if(!deadline) deadline=now+1;
    const int remaining=int(deadline-now);
    if(remaining>0) while(int(deadline-sceDisplayGetVcount())>0) sceDisplayWaitVblankStart();
    now=sceDisplayGetVcount();deadline+=2;
    if(int(now-deadline)>=0) deadline=now+2;
}
