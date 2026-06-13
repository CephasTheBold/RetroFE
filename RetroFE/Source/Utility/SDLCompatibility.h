#pragma once

#ifndef SDL_ENABLE_OLD_NAMES
#define SDL_ENABLE_OLD_NAMES
#endif

#if __has_include(<SDL3/SDL.h>)
#define RETROFE_USING_SDL3 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_oldnames.h>
#elif __has_include(<SDL2/SDL.h>)
#define RETROFE_USING_SDL3 0
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#define RETROFE_USING_SDL3 0
#include <SDL.h>
#else
#error "Cannot find SDL header"
#endif

inline void RetroFE_FreeSurfaceCompat(SDL_Surface* surface) {
    if (!surface) return;
#if RETROFE_USING_SDL3
    SDL_DestroySurface(surface);
#else
    SDL_FreeSurface(surface);
#endif
}

inline SDL_Surface* RetroFE_ConvertSurfaceFormatCompat(SDL_Surface* surface, Uint32 pixelFormat) {
#if RETROFE_USING_SDL3
    return surface ? SDL_ConvertSurface(surface, static_cast<SDL_PixelFormat>(pixelFormat)) : nullptr;
#else
    return surface ? SDL_ConvertSurfaceFormat(surface, pixelFormat, 0) : nullptr;
#endif
}

inline SDL_Surface* RetroFE_CreateSurfaceWithFormatCompat(int width, int height, Uint32 pixelFormat) {
#if RETROFE_USING_SDL3
    return SDL_CreateSurface(width, height, static_cast<SDL_PixelFormat>(pixelFormat));
#else
    return SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(pixelFormat), pixelFormat);
#endif
}

inline SDL_Surface* RetroFE_CreateRGBSurfaceCompat(int width, int height) {
    return RetroFE_CreateSurfaceWithFormatCompat(width, height, SDL_PIXELFORMAT_ARGB8888);
}

inline Uint32 RetroFE_MapRGBACompat(SDL_PixelFormat* format, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    return SDL_MapRGBA(format, r, g, b, a);
}

#if RETROFE_USING_SDL3
inline Uint32 RetroFE_MapRGBACompat(SDL_PixelFormat format, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(format);
    return details ? SDL_MapRGBA(details, nullptr, r, g, b, a) : 0;
}
#endif

inline void RetroFE_GetRGBACompat(Uint32 pixel, SDL_PixelFormat* format, Uint8* r, Uint8* g, Uint8* b, Uint8* a) {
    SDL_GetRGBA(pixel, format, r, g, b, a);
}

#if RETROFE_USING_SDL3
inline void RetroFE_GetRGBACompat(Uint32 pixel, SDL_PixelFormat format, Uint8* r, Uint8* g, Uint8* b, Uint8* a) {
    const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(format);
    if (details) {
        SDL_GetRGBA(pixel, details, nullptr, r, g, b, a);
    } else {
        if (r) *r = 0;
        if (g) *g = 0;
        if (b) *b = 0;
        if (a) *a = 0;
    }
}
#endif

inline int RetroFE_SurfaceBytesPerPixel(const SDL_Surface* surface) {
    if (!surface) return 0;
#if RETROFE_USING_SDL3
    const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(surface->format);
    return details ? details->bytes_per_pixel : 0;
#else
    return surface->format ? surface->format->BytesPerPixel : 0;
#endif
}

inline Uint32 RetroFE_SurfaceAlphaMask(const SDL_Surface* surface) {
    if (!surface) return 0;
#if RETROFE_USING_SDL3
    const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(surface->format);
    return details ? details->Amask : 0;
#else
    return surface->format ? surface->format->Amask : 0;
#endif
}

inline int RetroFE_SurfaceAlphaShift(const SDL_Surface* surface) {
    if (!surface) return 0;
#if RETROFE_USING_SDL3
    const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(surface->format);
    return details ? details->Ashift : 0;
#else
    return surface->format ? surface->format->Ashift : 0;
#endif
}

#if RETROFE_USING_SDL3
#define SDL_FreeSurface RetroFE_FreeSurfaceCompat
#define SDL_ConvertSurfaceFormat(surface, format, flags) RetroFE_ConvertSurfaceFormatCompat(surface, format)
#define SDL_CreateRGBSurfaceWithFormat(flags, width, height, depth, format) RetroFE_CreateSurfaceWithFormatCompat(width, height, format)
#define SDL_CreateRGBSurface(flags, width, height, depth, rmask, gmask, bmask, amask) RetroFE_CreateRGBSurfaceCompat(width, height)
#define SDL_MapRGBA(format, r, g, b, a) RetroFE_MapRGBACompat(format, r, g, b, a)
#define SDL_GetRGBA(pixel, format, r, g, b, a) RetroFE_GetRGBACompat(pixel, format, r, g, b, a)
#endif
