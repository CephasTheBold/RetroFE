#pragma once

#include "SDLCompatibility.h"

#if __has_include(<SDL2_mixer/SDL_mixer.h>)
#include <SDL2_mixer/SDL_mixer.h>
#elif __has_include(<SDL_mixer.h>)
#include <SDL_mixer.h>
#else
#error "Cannot find SDL_mixer header"
#endif
