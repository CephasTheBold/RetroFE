#ifndef RETROFE_IMAGE_H
#define RETROFE_IMAGE_H

#include "Component.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <atomic>
#include <mutex>
#include <memory>
#include <SDL2/SDL_image.h>

struct SDL_Texture;
struct SDL_Surface;
struct SDL_RWops;

class Image : public Component {
public:
    Image(const std::string& file, const std::string& altFile, Page& p,
        int monitor = 0, bool additive = false, bool useTextureCaching = true);
    ~Image() override;

    void draw() override;
    void allocateGraphicsMemory() override;
    void freeGraphicsMemory() override;
    std::string_view filePath();

    static void cleanupTextureCache();

private:
    struct CachedImage {
        SDL_Texture* texture = nullptr;
        int width = 0;
        int height = 0;
        std::vector<SDL_Surface*> animatedSurfaces;
        std::vector<int> frameDelays;
    };

    struct PathCache {
        struct CacheKey {
            std::string_view fullPath;
            int monitor;
            bool operator==(const CacheKey& other) const {
                return monitor == other.monitor && fullPath == other.fullPath;
            }
        };

        struct CacheKeyHash {
            size_t operator()(const CacheKey& k) const {
                return std::hash<std::string_view>{}(k.fullPath) ^ (std::hash<int>{}(k.monitor) << 1);
            }
        };

        std::unordered_set<std::string> fullPaths_;
        CacheKey getKey(const std::string& filePath, int monitor);
    };

    struct LoadResult {
        bool isCacheHit = false;
        SDL_Texture* cachedTexture = nullptr;

        SDL_Surface* surface = nullptr;
        std::vector<SDL_Surface*> animatedSurfaces;
        std::vector<int> frameDelays;

        float width = 0;
        float height = 0;
    };

    // Lifetime bridge: stays alive via shared_ptr even if Image is destroyed
    struct SharedLoadState {
        std::atomic<LoadResult*> pendingResult{ nullptr };
        std::atomic<bool> isLoading{ false };

        ~SharedLoadState() {
            if (LoadResult* res = pendingResult.exchange(nullptr)) {
                // If the Image was deleted before we could draw, clean up the result
                if (res->surface) SDL_FreeSurface(res->surface);
                for (auto* s : res->animatedSurfaces) if (s) SDL_FreeSurface(s);
                delete res;
            }
        }
    };

    void resetAnimationState();
    void clearInstanceResourcesForRetry();
    bool createAnimatedStreamingTexture(int width, int height);
    void primeAnimatedTextureIfNeeded();
    static void ensureCacheReserved();

    std::string file_;
    std::string altFile_;

    SDL_Texture* texture_ = nullptr;
    SDL_Texture* animatedTexture_ = nullptr;
    std::vector<SDL_Surface*> animatedSurfaces_;
    std::vector<int> frameDelays_;

    int currentFrame_ = 0;
    Uint32 lastFrameTime_ = 0;

    bool useTextureCaching_;
    bool isUsingCachedStaticTexture_ = false;
    bool isUsingCachedSurfaces_ = false;

    std::shared_ptr<SharedLoadState> loadState_;

    static PathCache pathCache_;
    static std::unordered_map<PathCache::CacheKey, CachedImage, PathCache::CacheKeyHash> textureCache_;
    static std::mutex textureCacheMutex_;
};

#endif