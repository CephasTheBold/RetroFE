#include "Image.h"
#include "../ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include "../../Utility/ThreadPool.h"

// -------------------- Static Storage --------------------
Image::PathCache Image::pathCache_;
std::unordered_map<Image::PathCache::CacheKey, Image::CachedImage, Image::PathCache::CacheKeyHash> Image::textureCache_;
std::mutex Image::textureCacheMutex_;

Image::PathCache::CacheKey Image::PathCache::getKey(const std::string& filePath, int monitor) {
    const auto& interned = *fullPaths_.emplace(filePath).first;
    return { interned, monitor };
}

void Image::ensureCacheReserved() {
    static bool reserved = false;
    if (reserved) return;
    textureCache_.reserve(4096);
    reserved = true;
}

// -------------------- Lifecycle --------------------
Image::Image(const std::string& file, const std::string& altFile, Page& p, int monitor, bool additive, bool useTextureCaching)
    : Component(p), file_(file), altFile_(altFile), useTextureCaching_(useTextureCaching) {
    baseViewInfo.Monitor = monitor;
    baseViewInfo.Additive = additive;
    baseViewInfo.Layout = page.getCurrentLayout();
}

Image::~Image() {
    if (LoadResult* res = pendingResult_.exchange(nullptr)) {
        if (!res->isCacheHit) {
            if (res->surface) SDL_FreeSurface(res->surface);
            for (auto* s : res->animatedSurfaces) if (s) SDL_FreeSurface(s);
        }
        delete res;
    }
    freeGraphicsMemory();
}

// -------------------- Memory Management --------------------
void Image::allocateGraphicsMemory() {
    if (texture_ || animatedTexture_ || isLoading_) return;
    if (useTextureCaching_) ensureCacheReserved();
    if (file_.empty()) return;

    isLoading_ = true;
    std::string filePath = file_;
    int monitor = baseViewInfo.Monitor;
    bool useCache = useTextureCaching_;

    ThreadPool::getInstance().enqueue([this, filePath, monitor, useCache]() {
        auto result = std::make_unique<LoadResult>();

        if (useCache) {
            std::scoped_lock lock(textureCacheMutex_);
            auto cacheKey = pathCache_.getKey(filePath, monitor);
            auto it = textureCache_.find(cacheKey);
            if (it != textureCache_.end()) {
                result->isCacheHit = true;
                result->cachedTexture = it->second.texture;
                result->width = (float)it->second.width;   // Metadata from RAM
                result->height = (float)it->second.height; // Metadata from RAM
                result->animatedSurfaces = it->second.animatedSurfaces;
                result->frameDelays = it->second.frameDelays;
                pendingResult_.store(result.release());
                return;
            }
        }

        SDL_RWops* rw = SDL_RWFromFile(filePath.c_str(), "rb");
        if (!rw) {
            isLoading_ = false;
            return;
        }

        if (IMG_isGIF(rw) || IMG_isWEBP(rw)) {
            IMG_Animation* anim = IMG_LoadAnimation_RW(rw, 1);
            if (anim) {
                result->width = (float)anim->w;
                result->height = (float)anim->h;
                for (int i = 0; i < anim->count; ++i) {
                    SDL_Surface* s = SDL_ConvertSurfaceFormat(anim->frames[i], SDL_PIXELFORMAT_RGBA32, 0);
                    if (s) {
                        result->animatedSurfaces.push_back(s);
                        result->frameDelays.push_back((anim->delays && anim->delays[i] > 0) ? anim->delays[i] : 100);
                    }
                }
                IMG_FreeAnimation(anim);
            }
        }
        else {
            SDL_Surface* temp = IMG_Load_RW(rw, 1);
            if (temp) {
                result->surface = SDL_ConvertSurfaceFormat(temp, SDL_PIXELFORMAT_RGBA32, 0);
                result->width = (float)result->surface->w;
                result->height = (float)result->surface->h;
                SDL_FreeSurface(temp);
            }
        }
        pendingResult_.store(result.release());
        });
}

// -------------------- Draw --------------------
void Image::draw() {
    Component::draw();

    if (LoadResult* res = pendingResult_.exchange(nullptr)) {
        if (res->isCacheHit) {
            texture_ = res->cachedTexture;
            isUsingCachedStaticTexture_ = (texture_ != nullptr);
            animatedSurfaces_ = res->animatedSurfaces;
            frameDelays_ = res->frameDelays;
            isUsingCachedSurfaces_ = !animatedSurfaces_.empty();
            if (isUsingCachedSurfaces_) {
                createAnimatedStreamingTexture((int)res->width, (int)res->height);
                primeAnimatedTextureIfNeeded();
            }
        }
        else {
            if (res->surface) {
                texture_ = SDL_CreateTextureFromSurface(SDL::getRenderer(baseViewInfo.Monitor), res->surface);
                SDL_FreeSurface(res->surface);
            }
            else if (!res->animatedSurfaces.empty()) {
                animatedSurfaces_ = res->animatedSurfaces;
                frameDelays_ = res->frameDelays;
                createAnimatedStreamingTexture((int)res->width, (int)res->height);
                primeAnimatedTextureIfNeeded();
            }

            if (useTextureCaching_) {
                std::scoped_lock lock(textureCacheMutex_);
                auto cacheKey = pathCache_.getKey(file_, baseViewInfo.Monitor);
                CachedImage ci;
                ci.texture = texture_;
                ci.width = (int)res->width;   // Save dimensions for next lookup
                ci.height = (int)res->height; // Save dimensions for next lookup
                ci.animatedSurfaces = animatedSurfaces_;
                ci.frameDelays = frameDelays_;
                textureCache_[cacheKey] = ci;
                isUsingCachedStaticTexture_ = (texture_ != nullptr);
                isUsingCachedSurfaces_ = !animatedSurfaces_.empty();
            }
        }
        baseViewInfo.ImageWidth = res->width;
        baseViewInfo.ImageHeight = res->height;
        isLoading_ = false;
        delete res;
    }

    if (!texture_ && !animatedTexture_) return;

    if (animatedTexture_ && !animatedSurfaces_.empty() && !frameDelays_.empty()) {
        Uint32 now = SDL_GetTicks();
        if (lastFrameTime_ == 0) lastFrameTime_ = now;

        bool frameChanged = false;
        size_t maxAdvance = animatedSurfaces_.size();
        size_t advanced = 0;

        while (now - lastFrameTime_ >= (Uint32)frameDelays_[currentFrame_] && advanced < maxAdvance) {
            lastFrameTime_ += (Uint32)frameDelays_[currentFrame_];
            currentFrame_ = (currentFrame_ + 1) % animatedSurfaces_.size();
            frameChanged = true;
            advanced++;
        }
        if (now - lastFrameTime_ > 1000) lastFrameTime_ = now;

        if (frameChanged) {
            SDL_Surface* s = animatedSurfaces_[currentFrame_];
            if (s && s->pixels) SDL_UpdateTexture(animatedTexture_, nullptr, s->pixels, s->pitch);
        }
    }

    SDL_Texture* target = animatedTexture_ ? animatedTexture_ : texture_;
    SDL_SetTextureBlendMode(target, baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);

    SDL_FRect rect{ baseViewInfo.XRelativeToOrigin(), baseViewInfo.YRelativeToOrigin(), baseViewInfo.ScaledWidth(), baseViewInfo.ScaledHeight() };
    SDL::renderCopyF(target, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
        page.getLayoutWidthByMonitor(baseViewInfo.Monitor), page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
}

// -------------------- Helpers --------------------
void Image::freeGraphicsMemory() {
    if (animatedTexture_) SDL_DestroyTexture(animatedTexture_);
    if (texture_ && !isUsingCachedStaticTexture_) SDL_DestroyTexture(texture_);
    if (!animatedSurfaces_.empty() && !isUsingCachedSurfaces_) {
        for (auto* s : animatedSurfaces_) if (s) SDL_FreeSurface(s);
    }
    texture_ = nullptr;
    animatedTexture_ = nullptr;
    animatedSurfaces_.clear();
    frameDelays_.clear();
    isUsingCachedStaticTexture_ = isUsingCachedSurfaces_ = false;
    resetAnimationState();
}

void Image::resetAnimationState() {
    currentFrame_ = 0;
    lastFrameTime_ = 0;
}

void Image::clearInstanceResourcesForRetry() { freeGraphicsMemory(); }

bool Image::createAnimatedStreamingTexture(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    animatedTexture_ = SDL_CreateTexture(SDL::getRenderer(baseViewInfo.Monitor), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
    return animatedTexture_ != nullptr;
}

void Image::primeAnimatedTextureIfNeeded() {
    if (animatedTexture_ && !animatedSurfaces_.empty()) {
        SDL_Surface* s = animatedSurfaces_[0];
        if (s) SDL_UpdateTexture(animatedTexture_, nullptr, s->pixels, s->pitch);
    }
}

void Image::cleanupTextureCache() {
    std::scoped_lock lock(textureCacheMutex_);
    for (auto& [key, entry] : textureCache_) {
        if (entry.texture) SDL_DestroyTexture(entry.texture);
        for (auto* s : entry.animatedSurfaces) if (s) SDL_FreeSurface(s);
    }
    textureCache_.clear();
}

std::string_view Image::filePath() { return file_; }