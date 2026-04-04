#ifndef RETROFE_IMAGE_H
#define RETROFE_IMAGE_H

#include "Component.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
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

    struct LoadContext {
        const std::string& filePath;
        PathCache::CacheKey cacheKey;
        CachedImage& newCachedImage;
        ViewInfo& baseViewInfo;
        bool useCache;
    };

    bool loadStaticImage(SDL_RWops* rw, LoadContext& ctx);
    bool loadAnimatedImage(SDL_RWops* rw, LoadContext& ctx);
    bool loadFromCache(LoadContext& ctx);

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
    Uint32 lastFrameTime_ = 0; // 0 indicates the animation hasn't started yet

    bool useTextureCaching_;
    bool isUsingCachedStaticTexture_ = false;
    bool isUsingCachedSurfaces_ = false;

    static PathCache pathCache_;
    static std::unordered_map<PathCache::CacheKey, CachedImage, PathCache::CacheKeyHash> textureCache_;
};

#endif