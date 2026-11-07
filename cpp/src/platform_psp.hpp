#pragma once
#include <SDL.h>
void psp_poll_controls();
bool psp_button_held(SDL_GameControllerButton button);

unsigned psp_configure_fpu();
unsigned psp_read_fpu_control();

void psp_pace_frame_30fps();
