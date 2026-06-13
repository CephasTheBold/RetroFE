#pragma once

#include "SDLCompatibility.h"

#if __has_include(<SDL3_ttf/SDL_ttf.h>)
#define RETROFE_USING_SDL3_TTF 1
#include <SDL3_ttf/SDL_ttf.h>
#elif __has_include(<SDL2_ttf/SDL_ttf.h>)
#define RETROFE_USING_SDL3_TTF 0
#include <SDL2_ttf/SDL_ttf.h>
#elif __has_include(<SDL2/SDL_ttf.h>)
#define RETROFE_USING_SDL3_TTF 0
#include <SDL2/SDL_ttf.h>
#elif __has_include(<SDL_ttf.h>)
#define RETROFE_USING_SDL3_TTF 0
#include <SDL_ttf.h>
#else
#error "Cannot find SDL_ttf header"
#endif

inline int RetroFE_TTF_InitCompat() {
#if RETROFE_USING_SDL3_TTF
    return TTF_Init() ? 0 : -1;
#else
    return TTF_Init();
#endif
}

inline const char* RetroFE_TTF_GetErrorCompat() {
#if RETROFE_USING_SDL3_TTF
    return SDL_GetError();
#else
    return TTF_GetError();
#endif
}

inline int RetroFE_TTF_GlyphIsProvided32Compat(TTF_Font* font, Uint32 ch) {
#if RETROFE_USING_SDL3_TTF
    return TTF_FontHasGlyph(font, ch) ? 1 : 0;
#else
    return TTF_GlyphIsProvided32(font, ch);
#endif
}

inline int RetroFE_TTF_GlyphMetricsCompat(TTF_Font* font, Uint32 ch, int* minx, int* maxx, int* miny, int* maxy, int* advance) {
#if RETROFE_USING_SDL3_TTF
    return TTF_GetGlyphMetrics(font, ch, minx, maxx, miny, maxy, advance) ? 0 : -1;
#else
    return TTF_GlyphMetrics(font, ch, minx, maxx, miny, maxy, advance);
#endif
}

inline SDL_Surface* RetroFE_TTF_RenderGlyph32_BlendedCompat(TTF_Font* font, Uint32 ch, SDL_Color fg) {
#if RETROFE_USING_SDL3_TTF
    return TTF_RenderGlyph_Blended(font, ch, fg);
#else
    return TTF_RenderGlyph32_Blended(font, ch, fg);
#endif
}

inline SDL_Surface* RetroFE_TTF_RenderText_SolidCompat(TTF_Font* font, const char* text, SDL_Color fg) {
#if RETROFE_USING_SDL3_TTF
    return TTF_RenderText_Solid(font, text, text ? SDL_strlen(text) : 0, fg);
#else
    return TTF_RenderText_Solid(font, text, fg);
#endif
}

inline int RetroFE_TTF_GetFontKerningSizeGlyphs32Compat(TTF_Font* font, Uint32 previous_ch, Uint32 ch) {
#if RETROFE_USING_SDL3_TTF
    int kerning = 0;
    return TTF_GetGlyphKerning(font, previous_ch, ch, &kerning) ? kerning : 0;
#else
    return TTF_GetFontKerningSizeGlyphs32(font, previous_ch, ch);
#endif
}

#if RETROFE_USING_SDL3_TTF
#define TTF_Init() RetroFE_TTF_InitCompat()
#define TTF_GetError() RetroFE_TTF_GetErrorCompat()
#define TTF_GlyphIsProvided32(font, ch) RetroFE_TTF_GlyphIsProvided32Compat(font, ch)
#define TTF_GlyphMetrics(font, ch, minx, maxx, miny, maxy, advance) RetroFE_TTF_GlyphMetricsCompat(font, ch, minx, maxx, miny, maxy, advance)
#define TTF_RenderGlyph32_Blended(font, ch, fg) RetroFE_TTF_RenderGlyph32_BlendedCompat(font, ch, fg)
#define TTF_RenderText_Solid(font, text, fg) RetroFE_TTF_RenderText_SolidCompat(font, text, fg)
#define TTF_FontHeight(font) TTF_GetFontHeight(font)
#define TTF_FontAscent(font) TTF_GetFontAscent(font)
#define TTF_FontDescent(font) TTF_GetFontDescent(font)
#define TTF_GetFontKerningSizeGlyphs32(font, previous_ch, ch) RetroFE_TTF_GetFontKerningSizeGlyphs32Compat(font, previous_ch, ch)
#endif
