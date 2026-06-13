#pragma once

#include "SDLCompatibility.h"

#if __has_include(<SDL3/SDL_syswm.h>)
#include <SDL3/SDL_syswm.h>
#elif __has_include(<SDL2/SDL_syswm.h>)
#include <SDL2/SDL_syswm.h>
#elif __has_include(<SDL_syswm.h>)
#include <SDL_syswm.h>
#else
#error "Cannot find SDL_syswm header"
#endif
