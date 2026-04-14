#include "Image.h"
#include "../ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include <mutex>

// -------------------- Static Storage --------------------
Image::PathCache Image::pathCache_;
std::unordered_map<Image::PathCache::CacheKey, Image::CachedImage, Image::PathCache::CacheKeyHash> Image::textureCache_;
std::unordered_map<std::string, std::shared_future<Image::AsyncLoadResult>> Image::loadingTasks_;

// Global mutex to prevent multi-monitor/thread map corruption
static std::mutex g_ImageCacheMutex;

Image::PathCache::CacheKey Image::PathCache::getKey(const std::string& filePath, int monitor) {
    // Note: fullPaths_ insertion is protected by the mutex in startAsyncLoad
    const auto& interned = *fullPaths_.emplace(filePath).first;
    return { interned, monitor };
}

void Image::ensureCacheReserved() {
    static bool reserved = false;
    if (reserved) return;
    std::lock_guard<std::mutex> lock(g_ImageCacheMutex);
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

// -------------------- Async Logic --------------------
void Image::allocateGraphicsMemory() {
    if (status_ != LoadStatus::Unloaded) return;
    if (useTextureCaching_) ensureCacheReserved();

    // Strategy: Try primary file, if that doesn't exist/start, try alt
    if (!file_.empty() && startAsyncLoad(file_)) return;
    if (!altFile_.empty() && startAsyncLoad(altFile_)) return;

    status_ = LoadStatus::Error;
}

bool Image::startAsyncLoad(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_ImageCacheMutex);

    // 1. Check Monitor-Specific VRAM Cache
    if (useTextureCaching_ && loadFromCache(path)) {
        status_ = LoadStatus::Ready;
        return true;
    }

    // 2. Check for an already in-flight task for this path
    auto it = loadingTasks_.find(path);
    if (it != loadingTasks_.end()) {
        loadTask_ = it->second;
        currentLoadingPath_ = path;
        status_ = LoadStatus::Loading;
        return true;
    }

    // 3. Start a new decompression task
    currentLoadingPath_ = path;
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
    return true;
}

void Image::finalizeLoad() {
    std::string path = currentLoadingPath_;

    // Check VRAM again in case another monitor finished while we waited
    {
        std::lock_guard<std::mutex> lock(g_ImageCacheMutex);
        if (useTextureCaching_ && loadFromCache(path)) {
            status_ = LoadStatus::Ready;
            loadingTasks_.erase(path);
            return;
        }
    }

    AsyncLoadResult res = loadTask_.get();

    // Fallback logic: If primary failed, trigger alt load
    if (!res.success) {
        {
            std::lock_guard<std::mutex> lock(g_ImageCacheMutex);
            loadingTasks_.erase(path);
        }

        if (path == file_ && !altFile_.empty()) {
            status_ = LoadStatus::Unloaded;
            startAsyncLoad(altFile_);
            return;
        }

        status_ = LoadStatus::Error;
        return;
    }

    // Decompression succeeded, perform Main-Thread GPU Upload
    SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
    if (res.staticSurface) {
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
            std::lock_guard<std::mutex> lock(g_ImageCacheMutex);
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

    std::lock_guard<std::mutex> lock(g_ImageCacheMutex);
    loadingTasks_.erase(path);
}

// -------------------- Render Logic --------------------
bool Image::loadFromCache(const std::string& filePath) {
    // This helper assumes a mutex lock is already held by the caller
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

    if (texture_ || animatedTexture_) {
        baseViewInfo.ImageWidth = (float)ci.animatedSurfaces.empty() ? baseViewInfo.ImageWidth : (float)animatedSurfaces_[0]->w;
        // Logic for setting width/height from query or cache
        primeAnimatedTextureIfNeeded();
        return true;
    }
    return false;
}

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

    if (animatedTexture_ && !animatedSurfaces_.empty() && !frameDelays_.empty()) {
        Uint32 now = SDL_GetTicks();

        /* Initialize a stable timeline anchor once */
        if (animationStartTime_ == 0) {
            animationStartTime_ = now;
            lastRenderedFrame_ = std::numeric_limits<size_t>::max(); // force upload
        }

        /* Compute total cycle time */
        Uint32 totalCycleTime = 0;
        for (int d : frameDelays_) totalCycleTime += (Uint32)d;

        if (totalCycleTime > 0) {
            /* Resolve current phase in cycle */
            Uint32 t = (now - animationStartTime_) % totalCycleTime;

            /* Map phase -> frame index */
            Uint32 accum = 0;
            size_t frameIndex = 0;

            for (size_t i = 0; i < frameDelays_.size(); ++i) {
                accum += (Uint32)frameDelays_[i];
                if (t < accum) {
                    frameIndex = i;
                    break;
                }
            }

            currentFrame_ = frameIndex;

            /* Upload only if frame changed */
            if (currentFrame_ != lastRenderedFrame_) {
                SDL_Surface* s = animatedSurfaces_[currentFrame_].get();
                if (s && s->pixels) {
                    SDL_UpdateTexture(animatedTexture_, nullptr, s->pixels, s->pitch);
                }
                lastRenderedFrame_ = currentFrame_;
            }
        }
    }

    SDL_Texture* target = animatedTexture_ ? animatedTexture_ : texture_;
    if (!target) return;

    SDL_SetTextureBlendMode(target, baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);

    SDL_FRect rect{
        baseViewInfo.XRelativeToOrigin(),
        baseViewInfo.YRelativeToOrigin(),
        baseViewInfo.ScaledWidth(),
        baseViewInfo.ScaledHeight()
    };

    SDL::renderCopyF(
        target,
        baseViewInfo.Alpha,
        nullptr,
        &rect,
        baseViewInfo,
        page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
        page.getLayoutHeightByMonitor(baseViewInfo.Monitor)
    );
}

// -------------------- Helpers --------------------
void Image::freeGraphicsMemory() {
    if (status_ == LoadStatus::Loading && loadTask_.valid()) {
        loadTask_.wait(); // Safe destruction: wait for background thread to finish
    }

    if (animatedTexture_) SDL_DestroyTexture(animatedTexture_);
    if (texture_ && !isUsingCachedStaticTexture_) SDL_DestroyTexture(texture_);

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
    std::lock_guard<std::mutex> lock(g_ImageCacheMutex);
    for (auto& [key, entry] : textureCache_) {
        if (entry.texture) SDL_DestroyTexture(entry.texture);
    }
    textureCache_.clear();
    loadingTasks_.clear();
}

std::string_view Image::filePath() { return file_; }