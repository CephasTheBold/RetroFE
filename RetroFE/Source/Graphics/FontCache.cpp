/* This file is part of RetroFE.
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RetroFE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RetroFE.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "FontCache.h"
#include "../SDL.h"
#include "../Utility/Log.h"
#include "Font.h"
#if __has_include(<SDL_ttf.h>)
#include <SDL_ttf.h>
#elif __has_include(<SDL2_ttf/SDL_ttf.h>)
#include <SDL2_ttf/SDL_ttf.h>
#else
#error "Cannot find SDL_ttf header"
#endif
#include <sstream>
#include <memory>

FontCache::FontCache() = default;

FontCache::~FontCache() {
    deInitialize();
}

void FontCache::deInitialize() {
    fontFaceMap_.clear(); // With smart pointers, no need for explicit delete calls
    TTF_Quit();
}

bool FontCache::initialize() const {
    if (TTF_Init() == 0)
    {
        return true;
    }
    else
    {
        LOG_WARNING("FontCache", "TTF_Init failed: " + std::string(TTF_GetError()));
        return false;
    }
}

FontManager* FontCache::getFont(const std::string& fontPath, int maxFontSize, SDL_Color color, bool gradient, int outlinePx, int monitor) {
    // 1. First, check for a perfect, exact configuration match (Fast path)
    std::string exactKey = buildFontKey(fontPath, maxFontSize, color, gradient, outlinePx, monitor);
    auto it = fontFaceMap_.find(exactKey);
    if (it != fontFaceMap_.end()) {
        return it->second.get();
    }

    // 2. Fallback: Search for an existing larger font that can satisfy this request via mipmapping
    FontManager* bestMatch = nullptr;
    int closestLargerSize = std::numeric_limits<int>::max();

    for (const auto& [key, fontPtr] : fontFaceMap_) {
        // Verify all styling attributes match perfectly so we aren't mixing colors or outlines
        if (fontPtr->getOutlinePx() == outlinePx &&
            // Note: If you add accessor methods for color/gradient to FontManager, verify them here:
            // fontPtr->getFontPath() == fontPath && fontPtr->getMonitor() == monitor
            key.rfind(fontPath, 0) == 0) // Basic prefix path check matching our key structure
        {
            int existingSize = fontPtr->getMaxFontSize(); // Add a public getter for maxFontSize_ if missing in Font.h

            // Is it larger than what we need, but smaller than any other candidate we've seen?
            if (existingSize >= maxFontSize && existingSize < closestLargerSize) {
                closestLargerSize = existingSize;
                bestMatch = fontPtr.get();
            }
        }
    }

    if (bestMatch) {
        LOG_INFO("FontCache", "[PERF] Shared font hit! Using existing size " +
            std::to_string(closestLargerSize) + " to satisfy request for size " + std::to_string(maxFontSize));
        return bestMatch;
    }

    return nullptr;
}


// MODIFIED: Parameter renamed to maxFontSize.
std::string FontCache::buildFontKey(std::string font, int maxFontSize, SDL_Color color, bool gradient, int outlinePx, int monitor) {
    std::stringstream ss;
    // MODIFIED: The key now uses the maxFontSize to ensure uniqueness for different mipmap chains.
    ss << font << "_SIZE=" << maxFontSize << " RGB=" << color.r << "." << color.g << "." << color.b;
    ss << "_MONITOR=" << monitor;
    ss << (gradient ? "_GRADIENT" : "");
    ss << "_OUTLINE=" << outlinePx;
    return ss.str();
}

bool FontCache::loadFont(std::string fontPath, int maxFontSize, SDL_Color color, bool gradient, int outlinePx, int monitor) {
    // Check if we already have an identical or larger font available that satisfies this layout component
    if (getFont(fontPath, maxFontSize, color, gradient, outlinePx, monitor) != nullptr) {
        return true; // Short-circuit completely! A valid target asset handle is already warm
    }

    std::string key = buildFontKey(fontPath, maxFontSize, color, gradient, outlinePx, monitor);

    // Only compile a cold initialization pass if absolutely no larger matching variant exists
    auto font = std::make_unique<FontManager>(fontPath, maxFontSize, color, gradient, outlinePx, monitor);
    if (font->initialize()) {
        fontFaceMap_[key] = std::move(font);
        return true;
    }

    return false;
}