#include "Image.h"
#include "../ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"

// -------------------- Static Storage --------------------
Image::PathCache Image::pathCache_;
std::unordered_map<Image::PathCache::CacheKey, Image::CachedImage, Image::PathCache::CacheKeyHash> Image::textureCache_;
std::unordered_map<std::string, std::shared_future<Image::AsyncLoadResult>> Image::loadingTasks_;

Image::PathCache::CacheKey Image::PathCache::getKey(const std::string& filePath, int monitor) {
    const auto& interned = *fullPaths_.emplace(filePath).first;
    return { interned, monitor };
}

void Image::ensureCacheReserved() {
    static bool reserved = false;
    if (reserved) return;
    textureCache_.reserve(4096);
    loadingTasks_.reserve(512);
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
    freeGraphicsMemory();
}

// -------------------- Memory Management --------------------
void Image::allocateGraphicsMemory() {
    if (status_ != LoadStatus::Unloaded) return;
    if (useTextureCaching_) ensureCacheReserved();

    std::string path = file_.empty() ? altFile_ : file_;
    if (path.empty()) return;

    // 1. Check Monitor-Specific VRAM Cache (Fastest Path)
    if (useTextureCaching_ && loadFromCache(path)) {
        status_ = LoadStatus::Ready;
        return;
    }

    // 2. Check Pending Load Task (Path-only, handles multi-monitor redundancy)
    auto it = loadingTasks_.find(path);
    if (it != loadingTasks_.end()) {
        loadTask_ = it->second;
        status_ = LoadStatus::Loading;
        return;
    }

    // 3. New Load: Start Decompression Task
    status_ = LoadStatus::Loading;
    loadTask_ = ThreadPool::getInstance().enqueue([path]() -> AsyncLoadResult {
        AsyncLoadResult res;
        SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
        if (!rw) return res;

        if (IMG_isGIF(rw) || IMG_isWEBP(rw)) {
            IMG_Animation* anim = IMG_LoadAnimation_RW(rw, 1);
            if (anim) {
                res.w = anim->w; res.h = anim->h;
                for (int i = 0; i < anim->count; ++i) {
                    SDL_Surface* conv = SDL_ConvertSurfaceFormat(anim->frames[i], SDL_PIXELFORMAT_RGBA32, 0);
                    res.animatedSurfaces.emplace_back(conv, SurfaceDeleter());
                    res.frameDelays.push_back((anim->delays && anim->delays[i] > 0) ? anim->delays[i] : 100);
                }
                IMG_FreeAnimation(anim);
                res.success = !res.animatedSurfaces.empty();
            }
        }
        else {
            // Background libpng/stb decompression
            SDL_Surface* s = IMG_Load_RW(rw, 1);
            if (s) {
                res.staticSurface = SharedSurface(s, SurfaceDeleter());
                res.w = s->w; res.h = s->h;
                res.success = true;
            }
        }
        return res;
        }).share();

    loadingTasks_[path] = loadTask_;
}

void Image::finalizeLoad() {
    std::string path = file_.empty() ? altFile_ : file_;

    // Double-check VRAM cache in case another instance on this monitor finished first
    if (useTextureCaching_ && loadFromCache(path)) {
        status_ = LoadStatus::Ready;
        loadingTasks_.erase(path);
        return;
    }

    AsyncLoadResult res = loadTask_.get();
    if (!res.success) {
        status_ = LoadStatus::Error;
        loadingTasks_.erase(path);
        return;
    }

    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);

    if (res.staticSurface) {
        // Upload to monitor-specific VRAM
        texture_ = SDL_CreateTextureFromSurface(renderer, res.staticSurface.get());
    }
    else if (!res.animatedSurfaces.empty()) {
        animatedSurfaces_ = res.animatedSurfaces;
        frameDelays_ = res.frameDelays;
        createAnimatedStreamingTexture(res.w, res.h);
    }

    if (texture_ || animatedTexture_) {
        baseViewInfo.ImageWidth = (float)res.w;
        baseViewInfo.ImageHeight = (float)res.h;
        status_ = LoadStatus::Ready;

        if (useTextureCaching_) {
            auto key = pathCache_.getKey(path, baseViewInfo.Monitor);
            CachedImage ci;
            ci.texture = texture_;
            ci.animatedSurfaces = animatedSurfaces_;
            ci.frameDelays = frameDelays_;
            textureCache_[key] = ci;
            isUsingCachedStaticTexture_ = (texture_ != nullptr);
            isUsingCachedSurfaces_ = !animatedSurfaces_.empty();
        }
        primeAnimatedTextureIfNeeded();
    }
    else {
        status_ = LoadStatus::Error;
    }

    // Only the main thread touches this, so it's safe to erase
    loadingTasks_.erase(path);
}

bool Image::loadFromCache(const std::string& filePath) {
    auto it = textureCache_.find(pathCache_.getKey(filePath, baseViewInfo.Monitor));
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
        createAnimatedStreamingTexture((int)baseViewInfo.ImageWidth, (int)baseViewInfo.ImageHeight);
    }

    SDL_Texture* target = texture_ ? texture_ : animatedTexture_;
    if (target) {
        SDL_QueryTexture(target, nullptr, nullptr, (int*)&baseViewInfo.ImageWidth, (int*)&baseViewInfo.ImageHeight);
        primeAnimatedTextureIfNeeded();
        return true;
    }
    return false;
}

// -------------------- Draw --------------------
void Image::draw() {
    Component::draw();

    if (status_ == LoadStatus::Loading) {
        if (loadTask_.valid() && loadTask_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            finalizeLoad();
        }
        else {
            return;
        }
    }

    if (status_ != LoadStatus::Ready) return;

    if (animatedTexture_ && !animatedSurfaces_.empty()) {
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
            SDL_Surface* s = animatedSurfaces_[currentFrame_].get();
            if (s && s->pixels) {
                SDL_UpdateTexture(animatedTexture_, nullptr, s->pixels, s->pitch);
            }
        }
    }

    SDL_Texture* target = animatedTexture_ ? animatedTexture_ : texture_;
    if (!target) return;

    SDL_SetTextureBlendMode(target, baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);

    SDL_FRect rect{ baseViewInfo.XRelativeToOrigin(), baseViewInfo.YRelativeToOrigin(), baseViewInfo.ScaledWidth(), baseViewInfo.ScaledHeight() };
    SDL::renderCopyF(target, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
        page.getLayoutWidthByMonitor(baseViewInfo.Monitor), page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
}

// -------------------- Helpers --------------------
void Image::freeGraphicsMemory() {
    if (status_ == LoadStatus::Loading && loadTask_.valid()) {
        loadTask_.wait();
    }

    if (animatedTexture_) SDL_DestroyTexture(animatedTexture_);
    if (texture_ && !isUsingCachedStaticTexture_) SDL_DestroyTexture(texture_);

    // shared_ptr handles deletion of animatedSurfaces if we are the last ones using them
    texture_ = nullptr;
    animatedTexture_ = nullptr;
    animatedSurfaces_.clear();
    frameDelays_.clear();
    isUsingCachedStaticTexture_ = isUsingCachedSurfaces_ = false;
    status_ = LoadStatus::Unloaded;
    resetAnimationState();
}

void Image::resetAnimationState() {
    currentFrame_ = 0;
    lastFrameTime_ = 0;
}

bool Image::createAnimatedStreamingTexture(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    animatedTexture_ = SDL_CreateTexture(SDL::getRenderer(baseViewInfo.Monitor), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
    return animatedTexture_ != nullptr;
}

void Image::primeAnimatedTextureIfNeeded() {
    if (animatedTexture_ && !animatedSurfaces_.empty()) {
        SDL_Surface* s = animatedSurfaces_[0].get();
        if (s) SDL_UpdateTexture(animatedTexture_, nullptr, s->pixels, s->pitch);
    }
}

void Image::cleanupTextureCache() {
    for (auto& [key, entry] : textureCache_) {
        if (entry.texture) SDL_DestroyTexture(entry.texture);
    }
    textureCache_.clear();
    loadingTasks_.clear();
}

std::string_view Image::filePath() { return file_; }