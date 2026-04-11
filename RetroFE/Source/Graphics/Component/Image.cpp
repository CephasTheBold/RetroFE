#include "Image.h"
#include "../ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"

// -------------------- Static Storage --------------------
Image::PathCache Image::pathCache_;
std::unordered_map<Image::PathCache::CacheKey, Image::CachedImage, Image::PathCache::CacheKeyHash> Image::textureCache_;

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

Image::~Image() { freeGraphicsMemory(); }

// -------------------- Memory Management --------------------
void Image::allocateGraphicsMemory() {
    if (texture_ || animatedTexture_) return;
    if (useTextureCaching_) ensureCacheReserved();

    auto tryLoad = [this](const std::string& filePath) -> bool {
        if (filePath.empty()) return false;
        clearInstanceResourcesForRetry();

        CachedImage newCachedImage;
        PathCache::CacheKey cacheKey{};

        if (useTextureCaching_) {
            cacheKey = pathCache_.getKey(filePath, baseViewInfo.Monitor);
            LoadContext ctx{ filePath, cacheKey, newCachedImage, baseViewInfo, true };
            if (loadFromCache(ctx)) return true;
        }

        SDL_RWops* rw = SDL_RWFromFile(filePath.c_str(), "rb");
        if (!rw) return false;

        LoadContext ctx{ filePath, cacheKey, newCachedImage, baseViewInfo, useTextureCaching_ };
        bool success = (IMG_isGIF(rw) || IMG_isWEBP(rw)) ? loadAnimatedImage(rw, ctx) : loadStaticImage(rw, ctx);

        if (!success) return false;

        if (useTextureCaching_) {
            auto [it, inserted] = textureCache_.try_emplace(cacheKey);
            if (inserted) {
                it->second = std::move(newCachedImage);
                isUsingCachedStaticTexture_ = (texture_ != nullptr);
                isUsingCachedSurfaces_ = !animatedSurfaces_.empty();
            }
            else {
                if (texture_ && !isUsingCachedStaticTexture_) SDL_DestroyTexture(texture_);
                texture_ = it->second.texture;
                isUsingCachedStaticTexture_ = (texture_ != nullptr);
                animatedSurfaces_ = it->second.animatedSurfaces;
                frameDelays_ = it->second.frameDelays;
                isUsingCachedSurfaces_ = !animatedSurfaces_.empty();
                primeAnimatedTextureIfNeeded();
            }
        }
        return true;
        };

    if (!file_.empty() && tryLoad(file_)) return;
    if (!altFile_.empty() && tryLoad(altFile_)) return;
}

// -------------------- Loaders --------------------
bool Image::loadStaticImage(SDL_RWops* rw, LoadContext& ctx) {
    SDL_Texture* tex = IMG_LoadTexture_RW(SDL::getRenderer(baseViewInfo.Monitor), rw, 1);
    if (!tex) return false;
    SDL_QueryTexture(tex, nullptr, nullptr, (int*)&ctx.baseViewInfo.ImageWidth, (int*)&ctx.baseViewInfo.ImageHeight);
    texture_ = tex;
    if (ctx.useCache) ctx.newCachedImage.texture = tex;
    return true;
}

bool Image::loadAnimatedImage(SDL_RWops* rw, LoadContext& ctx) {
    resetAnimationState();
    IMG_Animation* anim = IMG_LoadAnimation_RW(rw, 1);
    if (!anim) return false;

    if (anim->count <= 1) {
        texture_ = SDL_CreateTextureFromSurface(SDL::getRenderer(baseViewInfo.Monitor), anim->frames[0]);
        ctx.baseViewInfo.ImageWidth = (float)anim->w;
        ctx.baseViewInfo.ImageHeight = (float)anim->h;
        if (ctx.useCache) ctx.newCachedImage.texture = texture_;
        IMG_FreeAnimation(anim);
        return texture_ != nullptr;
    }

    for (int i = 0; i < anim->count; ++i) {
        SDL_Surface* s = SDL_ConvertSurfaceFormat(anim->frames[i], SDL_PIXELFORMAT_RGBA32, 0);
        if (s) {
            animatedSurfaces_.push_back(s);
            frameDelays_.push_back((anim->delays && anim->delays[i] > 0) ? anim->delays[i] : 100);
        }
    }

    ctx.baseViewInfo.ImageWidth = (float)anim->w;
    ctx.baseViewInfo.ImageHeight = (float)anim->h;
    bool ok = createAnimatedStreamingTexture(anim->w, anim->h);

    if (ok && ctx.useCache) {
        ctx.newCachedImage.animatedSurfaces = animatedSurfaces_;
        ctx.newCachedImage.frameDelays = frameDelays_;
    }

    IMG_FreeAnimation(anim);
    primeAnimatedTextureIfNeeded();
    return ok;
}

bool Image::loadFromCache(LoadContext& ctx) {
    auto it = textureCache_.find(ctx.cacheKey);
    if (it == textureCache_.end()) return false;
    const CachedImage& ci = it->second;
    if (ci.texture) {
        texture_ = ci.texture;
        isUsingCachedStaticTexture_ = true;
    }
    else if (!ci.animatedSurfaces.empty()) {
        animatedSurfaces_ = ci.animatedSurfaces;
        frameDelays_ = ci.frameDelays;
        isUsingCachedSurfaces_ = true;
        createAnimatedStreamingTexture(animatedSurfaces_[0]->w, animatedSurfaces_[0]->h);
    }
    SDL_Texture* target = texture_ ? texture_ : animatedTexture_;
    if (target) {
        SDL_QueryTexture(target, nullptr, nullptr, (int*)&ctx.baseViewInfo.ImageWidth, (int*)&ctx.baseViewInfo.ImageHeight);
        primeAnimatedTextureIfNeeded();
    }
    return true;
}

// -------------------- Draw (The "Stutter-Free" Logic) --------------------
void Image::draw() {
    Component::draw();
    if (!texture_ && !animatedTexture_) return;

    if (animatedTexture_ && !animatedSurfaces_.empty() && !frameDelays_.empty()) {
        Uint32 now = SDL_GetTicks();

        /* FIX 1: Lazy Initialization
           Align the animation clock to the EXACT moment it is first drawn.
           This prevents 'lost time' during loading from causing a frame-skip. */
        if (lastFrameTime_ == 0) {
            lastFrameTime_ = now;
        }

        bool frameChanged = false;

        /* FIX 2: Loop Safety
           We limit the while loop to advance at most one full cycle.
           This prevents 'micro-stutters' where it skips Frame 0 during the loop-around. */
        size_t maxAdvance = animatedSurfaces_.size();
        size_t advanced = 0;

        while (now - lastFrameTime_ >= (Uint32)frameDelays_[currentFrame_] && advanced < maxAdvance) {
            lastFrameTime_ += (Uint32)frameDelays_[currentFrame_];
            currentFrame_ = (currentFrame_ + 1) % animatedSurfaces_.size();
            frameChanged = true;
            advanced++;
        }

        /* If we are still massively out of sync (e.g. PC went to sleep), snap to now */
        if (now - lastFrameTime_ > 1000) {
            lastFrameTime_ = now;
        }

        if (frameChanged) {
            SDL_Surface* s = animatedSurfaces_[currentFrame_];
            if (s && s->pixels) {
                SDL_UpdateTexture(animatedTexture_, nullptr, s->pixels, s->pitch);
            }
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
    lastFrameTime_ = 0; // Set to 0 so draw() can lazy-init it
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
    for (auto& [key, entry] : textureCache_) {
        if (entry.texture) SDL_DestroyTexture(entry.texture);
        for (auto* s : entry.animatedSurfaces) if (s) SDL_FreeSurface(s);
    }
    textureCache_.clear();
}

std::string_view Image::filePath() { return file_; }