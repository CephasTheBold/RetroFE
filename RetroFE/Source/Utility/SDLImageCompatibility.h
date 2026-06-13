#pragma once

#include "SDLCompatibility.h"

#if __has_include(<SDL3_image/SDL_image.h>)
#define RETROFE_USING_SDL3_IMAGE 1
#include <SDL3_image/SDL_image.h>
#elif __has_include(<SDL2_image/SDL_image.h>)
#define RETROFE_USING_SDL3_IMAGE 0
#include <SDL2_image/SDL_image.h>
#elif __has_include(<SDL2/SDL_image.h>)
#define RETROFE_USING_SDL3_IMAGE 0
#include <SDL2/SDL_image.h>
#elif __has_include(<SDL_image.h>)
#define RETROFE_USING_SDL3_IMAGE 0
#include <SDL_image.h>
#else
#error "Cannot find SDL_image header"
#endif

inline int RetroFE_IMG_InitCompat(int flags) {
#if RETROFE_USING_SDL3_IMAGE
    return flags;
#else
    return IMG_Init(flags);
#endif
}

inline const char* RetroFE_IMG_GetErrorCompat() {
#if RETROFE_USING_SDL3_IMAGE
    return SDL_GetError();
#else
    return IMG_GetError();
#endif
}

inline SDL_Surface* RetroFE_IMG_Load_RWCompat(SDL_RWops* rw, int freesrc) {
#if RETROFE_USING_SDL3_IMAGE
    return IMG_Load_IO(rw, freesrc != 0);
#else
    return IMG_Load_RW(rw, freesrc);
#endif
}

inline SDL_Texture* RetroFE_IMG_LoadTexture_RWCompat(SDL_Renderer* renderer, SDL_RWops* rw, int freesrc) {
#if RETROFE_USING_SDL3_IMAGE
    return IMG_LoadTexture_IO(renderer, rw, freesrc != 0);
#else
    return IMG_LoadTexture_RW(renderer, rw, freesrc);
#endif
}

inline IMG_Animation* RetroFE_IMG_LoadAnimation_RWCompat(SDL_RWops* rw, int freesrc) {
#if RETROFE_USING_SDL3_IMAGE
    return IMG_LoadAnimation_IO(rw, freesrc != 0);
#else
    return IMG_LoadAnimation_RW(rw, freesrc);
#endif
}

inline int RetroFE_IMG_SavePNGCompat(SDL_Surface* surface, const char* file) {
#if RETROFE_USING_SDL3_IMAGE
    return IMG_SavePNG(surface, file) ? 0 : -1;
#else
    return IMG_SavePNG(surface, file);
#endif
}

#if RETROFE_USING_SDL3_IMAGE
#define IMG_Init(flags) RetroFE_IMG_InitCompat(flags)
#define IMG_GetError() RetroFE_IMG_GetErrorCompat()
#define IMG_Load_RW(rw, freesrc) RetroFE_IMG_Load_RWCompat(rw, freesrc)
#define IMG_LoadTexture_RW(renderer, rw, freesrc) RetroFE_IMG_LoadTexture_RWCompat(renderer, rw, freesrc)
#define IMG_LoadAnimation_RW(rw, freesrc) RetroFE_IMG_LoadAnimation_RWCompat(rw, freesrc)
#define IMG_SavePNG(surface, file) RetroFE_IMG_SavePNGCompat(surface, file)
#endif
