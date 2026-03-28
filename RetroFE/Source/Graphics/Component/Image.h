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

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <cstdint>

#include "Component.h"
#include <SDL.h>

#if __has_include(<SDL_image.h>)
#include <SDL_image.h>
#elif __has_include(<SDL2_image/SDL_image.h>)
#include <SDL2_image/SDL_image.h>
#else
#error "Cannot find SDL_image header"
#endif

#ifdef __APPLE__
#include <webp/decode.h>
#include <webp/demux.h>
#elif defined(_WIN32)
#include <SDL_image.h>
#include <decode.h>
#include <demux.h>
#else
#include <webp/decode.h>
#include <webp/demux.h>
#endif

class Image : public Component {
public:
    Image(const std::string& file, const std::string& altFile, Page& p,
        int monitor, bool additive, bool useTextureCaching = false);
    ~Image() override;

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    void allocateGraphicsMemory() override;
    void freeGraphicsMemory() override;
    void draw() override;
    std::string_view filePath() override;

    static void cleanupTextureCache();

private:
    // Interning cache for file paths so CacheKey can store string_view without allocating per lookup.
    class PathCache {
    public:
        struct CacheKey {
            std::string_view fullPath;
            int monitor = 0;

            // Hidden friend (clang-tidy friendly), C++17-compatible.
            friend bool operator==(const CacheKey& a, const CacheKey& b) noexcept {
                return a.monitor == b.monitor && a.fullPath == b.fullPath;
            }
        };

        struct CacheKeyHash {
            size_t operator()(const CacheKey& key) const noexcept {
                size_t h1 = std::hash<std::string_view>{}(key.fullPath);
                size_t h2 = std::hash<int>{}(key.monitor);

                // hash combine
                size_t h = h1;
                h ^= (h2 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
                return h;
            }
        };

        CacheKey getKey(const std::string& filePath, int monitor);

        void reserve(size_t n) {
            fullPaths_.reserve(n);
        }

    private:
        std::unordered_set<std::string> fullPaths_;
    };

    struct CachedImage {
        // Static: cached GPU texture (shared across instances).
        SDL_Texture* texture = nullptr;

        // Animated: cached CPU frames + delay (shared across instances).
        int frameDelay = 0;
        std::vector<SDL_Surface*> animatedSurfaces;
    };

    struct LoadContext {
        const std::string& filePath;
        PathCache::CacheKey cacheKey;
        CachedImage& newCachedImage;
        ViewInfo& baseViewInfo;
        bool useCache;
    };

private:
    // Core loading helpers
    bool loadFromCache(const LoadContext& ctx);
    bool loadStaticImage(SDL_RWops* rw, LoadContext& ctx);
    bool loadAnimatedGIF(SDL_RWops* rw, LoadContext& ctx);
    bool loadAnimatedWebP(const std::vector<uint8_t>& buffer, LoadContext& ctx);

    // WebP helpers
    static bool isAnimatedWebP(const std::vector<uint8_t>& buffer);
    static bool loadFileToBuffer(const std::string& filePath, std::vector<uint8_t>& outBuffer);

    // Animation helpers
    void resetAnimationState();
    void clearInstanceResourcesForRetry();
    bool createAnimatedStreamingTexture(int width, int height);
    void primeAnimatedTextureIfNeeded();

    // Cache init
    static void ensureCacheReserved();

private:
    std::string file_;
    std::string altFile_;

    // Static GPU resource (may be cached/shared)
    SDL_Texture* texture_ = nullptr;
    bool isUsingCachedStaticTexture_ = false;

    // Animated GPU resource is ALWAYS per-instance
    SDL_Texture* animatedTexture_ = nullptr;

    // Animated CPU frames may be cached/shared
    std::vector<SDL_Surface*> animatedSurfaces_;
    bool isUsingCachedSurfaces_ = false;

    // Animation playback state
    size_t currentFrame_ = 0;
    Uint32 lastFrameTime_ = 0;
    int frameDelay_ = 0;

    bool useTextureCaching_ = false;

    // Cache storage
    static PathCache pathCache_;
    static std::unordered_map<PathCache::CacheKey, CachedImage, PathCache::CacheKeyHash> textureCache_;
};