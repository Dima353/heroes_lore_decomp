#pragma once
// Keyboard and controller input: directions and the button labels of each platform.
#include <SDL.h>
#include <algorithm>
#include <cstdlib>
#include "platform.hpp"

inline int direction_from_key(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP: return 1;
        case SDLK_DOWN: return 2;
        case SDLK_LEFT: return 3;
        case SDLK_RIGHT: return 4;
        default: return 0;
    }
}

// SDL uses positional Xbox names on this Switch backend. Translate once so
// all UI actions below use the labels printed on Nintendo controllers.
inline Uint8 labelled_controller_button(Uint8 button) {
#if HEROES_LORE_SWITCH
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: return SDL_CONTROLLER_BUTTON_B;
        case SDL_CONTROLLER_BUTTON_B: return SDL_CONTROLLER_BUTTON_A;
        case SDL_CONTROLLER_BUTTON_X: return SDL_CONTROLLER_BUTTON_Y;
        case SDL_CONTROLLER_BUTTON_Y: return SDL_CONTROLLER_BUTTON_X;
        default: break;
    }
#endif
    return button;
}

inline int direction_from_controller_button(Uint8 button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return 1;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 2;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 3;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 4;
        default: return 0;
    }
}

// Port: the left stick acts as the D-pad. The dominant axis wins, with hysteresis so a
// diagonal does not flicker between two directions.
inline int direction_from_stick(int x,int y,int current) {
    const auto along=[&](int direction) {
        return direction==1?-y:direction==2?y:direction==3?-x:direction==4?x:0;
    };
    const int ax=std::abs(x),ay=std::abs(y);
    const int candidate=ax>ay?(x<0?3:4):(y<0?1:2);
    const int strength=std::max(ax,ay);
    if(current && along(current)>11000 && strength*4<along(current)*5) return current;
    return strength>=17000?candidate:0;
}

inline Uint8 controller_button_for_direction(int direction) {
    static constexpr Uint8 buttons[]{SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
    return buttons[std::clamp(direction,1,4)-1];
}
