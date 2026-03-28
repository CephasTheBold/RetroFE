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

#include "Image.h"
#include "../ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"

#include <fstream>
#include <cstring>
#include <utility>

 // -------------------- Static storage --------------------

Image::PathCache Image::pathCache_;
std::unordered_map<Image::PathCache::CacheKey, Image::CachedImage, Image::PathCache::CacheKeyHash> Image::textureCache_;

// -------------------- PathCache --------------------

Image::PathCache::CacheKey Image::PathCache::getKey(const std::string& filePath, int monitor) {
    // Intern the full path so CacheKey can safely store a string_view.
    // Single-threaded: no locking needed.
    const auto& interned = *fullPaths_.emplace(filePath).first;

    CacheKey key;
    key.fullPath = interned;
    key.monitor = monitor;
    return key;
}

// -------------------- Image lifecycle --------------------

Image::Image(const std::string& file, const std::string& altFile, Page& p,
    int monitor, bool additive, bool useTextureCaching)
    : Component(p)
    , file_(file)
    , altFile_(altFile)
    , useTextureCaching_(useTextureCaching) {
    baseViewInfo.Monitor = monitor;
    baseViewInfo.Additive = additive;
    baseViewInfo.Layout = page.getCurrentLayout();
}

Image::~Image() {
    freeGraphicsMemory();
}

// -------------------- Cache init --------------------

void Image::ensureCacheReserved() {
    static bool reserved = false;
    if (reserved) return;

    // You told me 4096 is fine.
    constexpr size_t kReserve = 4096;

    textureCache_.reserve(kReserve);
    pathCache_.reserve(kReserve);

    reserved = true;
}

// -------------------- Instance cleanup helpers --------------------

void Image::resetAnimationState() {
    currentFrame_ = 0;
    lastFrameTime_ = 0;
    frameDelay_ = 0;
}

void Image::clearInstanceResourcesForRetry() {
    resetAnimationState();

    // Animated streaming texture is ALWAYS per-instance.
    if (animatedTexture_) {
        SDL_DestroyTexture(animatedTexture_);
        animatedTexture_ = nullptr;
    }

    // Animated surfaces: free only if instance-owned.
    if (!animatedSurfaces_.empty()) {
        if (!isUsingCachedSurfaces_) {
            for (auto* s : animatedSurfaces_) {
                if (s) SDL_DestroySurface(s);
            }
        }
        animatedSurfaces_.clear();
    }
    isUsingCachedSurfaces_ = false;

    // Static texture: destroy only if instance-owned.
    if (texture_) {
        if (!isUsingCachedStaticTexture_) {
            SDL_DestroyTexture(texture_);
        }
        texture_ = nullptr;
    }
    isUsingCachedStaticTexture_ = false;
}

bool Image::createAnimatedStreamingTexture(int width, int height) {
    if (width <= 0 || height <= 0) return false;

    if (animatedTexture_) {
        SDL_DestroyTexture(animatedTexture_);
        animatedTexture_ = nullptr;
    }

    animatedTexture_ = SDL_CreateTexture(
        SDL::getRenderer(baseViewInfo.Monitor),
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height);

    if (!animatedTexture_) return false;

    // Blend mode for animatedTexture_ can be set here, but we also set right before draw for safety.
    SDL_SetTextureBlendMode(animatedTexture_,
        baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);

    return true;
}

void Image::primeAnimatedTextureIfNeeded() {
    if (animatedSurfaces_.empty()) return;

    SDL_Surface* s = animatedSurfaces_[0];
    if (!s || !s->pixels) return;

    int texW = 0, texH = 0;
    bool needCreate = (animatedTexture_ == nullptr);

    if (!needCreate) {
        if (SDL_QueryTexture(animatedTexture_, nullptr, nullptr, &texW, &texH) != 0) {
            needCreate = true;
        }
        else if (texW != s->w || texH != s->h) {
            needCreate = true;
        }
    }

    if (needCreate) {
        if (!createAnimatedStreamingTexture(s->w, s->h)) return;
    }

    SDL_UpdateTexture(animatedTexture_, nullptr, s->pixels, s->pitch);
}

// -------------------- allocateGraphicsMemory --------------------

void Image::allocateGraphicsMemory() {
    // Early-out only if we have a valid renderable GPU resource.
    if (texture_ || animatedTexture_) return;

    if (useTextureCaching_) {
        ensureCacheReserved();
    }

    auto tryLoad = [this](const std::string& filePath) -> bool {
        if (filePath.empty()) return false;

        // Clean attempt: ensure no partial state.
        clearInstanceResourcesForRetry();

        // If caching is enabled, compute key and try cache first.
        CachedImage newCachedImage;
        PathCache::CacheKey cacheKey{};

        if (useTextureCaching_) {
            cacheKey = pathCache_.getKey(filePath, baseViewInfo.Monitor);
            LoadContext ctx{ filePath, cacheKey, newCachedImage, baseViewInfo, true };

            if (loadFromCache(ctx)) {
                // Cache hit may have animation; ensure streaming texture primed.
                if (animatedTexture_ && frameDelay_ > 0) {
                    primeAnimatedTextureIfNeeded();
                    lastFrameTime_ = SDL_GetTicks();
                }
                return true;
            }
        }

        // Miss (or caching disabled): load from disk.
        SDL_IOStream* rw = SDL_IOFromFile(filePath.c_str(), "rb");
        if (!rw) return false;

        // RAII for file rw unless we hand ownership to SDL_image with freesrc=1.
        struct IOCloser {
            void operator()(SDL_IOStream* p) const noexcept { if (p) SDL_CloseIO(p); }
        };
        std::unique_ptr<SDL_IOStream, IOCloser> rwFile(rw);

        bool success = false;

        // Prepare LoadContext for loaders. If caching disabled, ctx.useCache=false and cacheKey unused.
        LoadContext ctx{
            filePath,
            cacheKey,
            newCachedImage,
            baseViewInfo,
            useTextureCaching_
        };

        if (IMG_isWEBP(rwFile.get())) {
            // Need full buffer for WebP demux if animated
            std::vector<uint8_t> buffer;
            if (loadFileToBuffer(filePath, buffer)) {
                if (isAnimatedWebP(buffer)) {
                    success = loadAnimatedWebP(buffer, ctx);
                }
                else {
                    SDL_IOStream* memRw = SDL_IOFromConstMem(buffer.data(), (int)buffer.size());
                    success = loadStaticImage(memRw, ctx); // frees memRw via freesrc=1
                }
            }
            // rwFile closes automatically
        }
        else if (IMG_isGIF(rwFile.get())) {
            // Hand ownership to IMG_LoadAnimation_IO by releasing unique_ptr and using freesrc=1 inside loader
            SDL_IOStream* raw = rwFile.release();
            success = loadAnimatedGIF(raw, ctx); // loader closes raw
        }
        else {
            // Static (PNG/JPG/QOI/etc). Hand ownership to IMG_LoadTexture_IO by releasing and using freesrc=1.
            SDL_IOStream* raw = rwFile.release();
            success = loadStaticImage(raw, ctx); // loader closes raw
        }

        if (!success) return false;

        // Prime animation and start timer
        if (animatedTexture_ && frameDelay_ > 0) {
            primeAnimatedTextureIfNeeded();
            lastFrameTime_ = SDL_GetTicks();
        }

        // Cache store (single-threaded, but still use try_emplace properly).
        if (useTextureCaching_) {
            const bool hasStaticToCache = (newCachedImage.texture != nullptr);
            const bool hasAnimatedToCache = (!newCachedImage.animatedSurfaces.empty());

            if (hasStaticToCache || hasAnimatedToCache) {
                // IMPORTANT: do not move newCachedImage into try_emplace args (would move even on hit).
                auto [it, inserted] = textureCache_.try_emplace(cacheKey);
                if (inserted) {
                    it->second = std::move(newCachedImage);

                    // Since cache now owns these, mark instance as borrowing shared resources.
                    if (hasStaticToCache) {
                        isUsingCachedStaticTexture_ = true;
                    }
                    if (hasAnimatedToCache) {
                        isUsingCachedSurfaces_ = true;
                    }
                }
                else {
                    // In single-thread this should be rare (only if cacheKey duplicates due to caller bugs),
                    // but handle robustly: prefer existing entry, clean up duplicates.
                    CachedImage& cached = it->second;

                    if (hasStaticToCache) {
                        // We loaded a new static texture but cache already has one.
                        // Destroy ours (instance-owned) and bind to cached.
                        if (texture_ && !isUsingCachedStaticTexture_) {
                            SDL_DestroyTexture(texture_);
                        }
                        texture_ = cached.texture;
                        isUsingCachedStaticTexture_ = (texture_ != nullptr);

                        // Ensure duplicate newCachedImage texture doesn't leak if different pointer.
                        if (newCachedImage.texture && newCachedImage.texture != cached.texture) {
                            SDL_DestroyTexture(newCachedImage.texture);
                            newCachedImage.texture = nullptr;
                        }
                    }

                    if (hasAnimatedToCache) {
                        // Destroy newly decoded surfaces from newCachedImage if we didn't adopt them.
                        for (auto* s : newCachedImage.animatedSurfaces) {
                            if (s) SDL_DestroySurface(s);
                        }
                        newCachedImage.animatedSurfaces.clear();

                        animatedSurfaces_ = cached.animatedSurfaces;
                        frameDelay_ = cached.frameDelay;
                        isUsingCachedSurfaces_ = !animatedSurfaces_.empty();

                        primeAnimatedTextureIfNeeded();
                        lastFrameTime_ = SDL_GetTicks();
                    }
                }
            }
        }

        return true;
        };

    if (!file_.empty() && tryLoad(file_)) return;
    if (!altFile_.empty() && tryLoad(altFile_)) return;

    LOG_ERROR("Image", "Failed to load image: " + file_ + (altFile_.empty() ? "" : (" | alt: " + altFile_)));
}

// -------------------- Loading helpers --------------------

bool Image::loadStaticImage(SDL_IOStream* rw, LoadContext& ctx) {
    // freesrc=1 => IMG_LoadTexture_IO closes rw
    SDL_Texture* tex = IMG_LoadTexture_IO(SDL::getRenderer(baseViewInfo.Monitor), rw, 1);
    if (!tex) return false;

    int w = 0, h = 0;
    if (SDL_QueryTexture(tex, nullptr, nullptr, &w, &h) != 0) {
        SDL_DestroyTexture(tex);
        return false;
    }

    ctx.baseViewInfo.ImageWidth = (float)w;
    ctx.baseViewInfo.ImageHeight = (float)h;

    texture_ = tex;

    if (ctx.useCache) {
        ctx.newCachedImage.texture = tex;
        // ownership marking happens when insertion succeeds
    }

    return true;
}

bool Image::loadAnimatedGIF(SDL_IOStream* rw, LoadContext& ctx) {
    resetAnimationState();

    // freesrc=1 => IMG_LoadAnimation_IO closes rw
    IMG_Animation* anim = IMG_LoadAnimation_IO(rw, 1);
    if (!anim) return false;

    // Single-frame GIF: treat as static texture
    if (anim->count <= 1) {
        SDL_Texture* tex = SDL_CreateTextureFromSurface(SDL::getRenderer(baseViewInfo.Monitor), anim->frames[0]);
        IMG_FreeAnimation(anim);
        if (!tex) return false;

        int w = 0, h = 0;
        if (SDL_QueryTexture(tex, nullptr, nullptr, &w, &h) != 0) {
            SDL_DestroyTexture(tex);
            return false;
        }

        ctx.baseViewInfo.ImageWidth = (float)w;
        ctx.baseViewInfo.ImageHeight = (float)h;

        texture_ = tex;

        if (ctx.useCache) {
            ctx.newCachedImage.texture = tex;
        }

        return true;
    }

    // Decode frames into a local vector first (no half-loaded state)
    std::vector<SDL_Surface*> decoded;
    decoded.reserve((size_t)anim->count);

    for (int i = 0; i < anim->count; ++i) {
        SDL_Surface* s = SDL_ConvertSurface(anim->frames[i], SDL_PIXELFORMAT_RGBA32);
        if (s) decoded.push_back(s);
    }

    int delay = 100;
    if (anim->delays && anim->count > 0 && anim->delays[0] > 0) delay = anim->delays[0];

    const int w = anim->w;
    const int h = anim->h;

    IMG_FreeAnimation(anim);

    if (decoded.empty()) return false;

    if (!createAnimatedStreamingTexture(w, h)) {
        for (auto* s : decoded) if (s) SDL_DestroySurface(s);
        return false;
    }

    animatedSurfaces_ = std::move(decoded);
    frameDelay_ = delay;
    lastFrameTime_ = SDL_GetTicks();
    currentFrame_ = 0;

    ctx.baseViewInfo.ImageWidth = (float)w;
    ctx.baseViewInfo.ImageHeight = (float)h;

    primeAnimatedTextureIfNeeded();

    if (ctx.useCache) {
        ctx.newCachedImage.animatedSurfaces = animatedSurfaces_;
        ctx.newCachedImage.frameDelay = frameDelay_;
    }

    return true;
}

bool Image::loadAnimatedWebP(const std::vector<uint8_t>& buffer, LoadContext& ctx) {
    resetAnimationState();

    WebPData webpData{ buffer.data(), buffer.size() };
    WebPDemuxer* demux = WebPDemux(&webpData);
    if (!demux) return false;

    const int width = (int)WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
    const int height = (int)WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);

    SDL_Surface* canvas = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!canvas) {
        WebPDemuxDelete(demux);
        return false;
    }

    SDL_FillRect(canvas, nullptr, SDL_MapRGBA(canvas->format, 0, 0, 0, 0));

    std::vector<SDL_Surface*> decoded;
    decoded.reserve((size_t)WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT));

    int delay = 0;

    int prevDispose = WEBP_MUX_DISPOSE_NONE;
    SDL_Rect prevRect{ 0, 0, 0, 0 };

    WebPIterator iter;
    bool ok = (WebPDemuxGetFrame(demux, 1, &iter) != 0);

    while (ok) {
        if (prevDispose == WEBP_MUX_DISPOSE_BACKGROUND) {
            SDL_FillRect(canvas, &prevRect, SDL_MapRGBA(canvas->format, 0, 0, 0, 0));
        }

        SDL_Surface* fragment = SDL_CreateRGBSurfaceWithFormat(0, iter.width, iter.height, 32, SDL_PIXELFORMAT_RGBA32);
        if (fragment) {
            const bool decodedOk =
                (WebPDecodeRGBAInto(
                    iter.fragment.bytes, iter.fragment.size,
                    (uint8_t*)fragment->pixels,
                    fragment->pitch * fragment->h,
                    fragment->pitch) != nullptr);

            if (decodedOk) {
                SDL_SetSurfaceBlendMode(fragment,
                    (iter.blend_method == WEBP_MUX_BLEND) ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);

                SDL_Rect r{ iter.x_offset, iter.y_offset, iter.width, iter.height };

                if (SDL_BlitSurface(fragment, nullptr, canvas, &r) == 0) {
                    SDL_Surface* frameCopy = SDL_ConvertSurface(canvas, canvas->format, 0);
                    if (frameCopy) decoded.push_back(frameCopy);
                }

                if (delay == 0) delay = (iter.duration > 0 ? iter.duration : 100);

                prevDispose = iter.dispose_method;
                prevRect = r;
            }

            SDL_DestroySurface(fragment);
        }

        ok = (WebPDemuxNextFrame(&iter) != 0);
    }

    WebPDemuxReleaseIterator(&iter);
    SDL_DestroySurface(canvas);
    WebPDemuxDelete(demux);

    if (decoded.empty()) return false;

    if (!createAnimatedStreamingTexture(width, height)) {
        for (auto* s : decoded) if (s) SDL_DestroySurface(s);
        return false;
    }

    animatedSurfaces_ = std::move(decoded);
    frameDelay_ = (delay > 0 ? delay : 100);
    lastFrameTime_ = SDL_GetTicks();
    currentFrame_ = 0;

    ctx.baseViewInfo.ImageWidth = (float)width;
    ctx.baseViewInfo.ImageHeight = (float)height;

    primeAnimatedTextureIfNeeded();

    if (ctx.useCache) {
        ctx.newCachedImage.animatedSurfaces = animatedSurfaces_;
        ctx.newCachedImage.frameDelay = frameDelay_;
    }

    return true;
}

// -------------------- Cache load --------------------

bool Image::loadFromCache(const LoadContext& ctx) {
    if (!ctx.useCache) return false;

    auto it = textureCache_.find(ctx.cacheKey);
    if (it == textureCache_.end()) return false;

    const CachedImage& ci = it->second;

    // Static cache hit
    if (ci.texture) {
        texture_ = ci.texture;
        isUsingCachedStaticTexture_ = true;

        int w = 0, h = 0;
        if (SDL_QueryTexture(texture_, nullptr, nullptr, &w, &h) == 0) {
            ctx.baseViewInfo.ImageWidth = (float)w;
            ctx.baseViewInfo.ImageHeight = (float)h;
        }
        return true;
    }

    // Animated cache hit (surfaces + delay)
    if (!ci.animatedSurfaces.empty() && ci.frameDelay > 0) {
        animatedSurfaces_ = ci.animatedSurfaces;
        frameDelay_ = ci.frameDelay;
        isUsingCachedSurfaces_ = true;

        SDL_Surface* first = animatedSurfaces_[0];
        if (!first) return false;

        if (!createAnimatedStreamingTexture(first->w, first->h)) {
            animatedSurfaces_.clear();
            frameDelay_ = 0;
            isUsingCachedSurfaces_ = false;
            return false;
        }

        ctx.baseViewInfo.ImageWidth = (float)first->w;
        ctx.baseViewInfo.ImageHeight = (float)first->h;

        currentFrame_ = 0;
        lastFrameTime_ = SDL_GetTicks();
        primeAnimatedTextureIfNeeded();

        return true;
    }

    return false;
}

// -------------------- WebP helpers --------------------

bool Image::loadFileToBuffer(const std::string& filePath, std::vector<uint8_t>& outBuffer) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file) return false;

    std::streamsize size = file.tellg();
    if (size <= 0) return false;

    file.seekg(0, std::ios::beg);
    outBuffer.resize((size_t)size);
    return (bool)file.read((char*)outBuffer.data(), size);
}

bool Image::isAnimatedWebP(const std::vector<uint8_t>& buffer) {
    WebPData webpData{ buffer.data(), buffer.size() };
    WebPDemuxer* demux = WebPDemux(&webpData);
    if (!demux) return false;

    int count = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
    WebPDemuxDelete(demux);
    return count > 1;
}

// -------------------- draw --------------------

void Image::draw() {
    Component::draw();

    if (!texture_ && !animatedTexture_) return;

    SDL_FRect rect{
        baseViewInfo.XRelativeToOrigin(),
        baseViewInfo.YRelativeToOrigin(),
        baseViewInfo.ScaledWidth(),
        baseViewInfo.ScaledHeight()
    };

    if (animatedTexture_ && frameDelay_ > 0 && !animatedSurfaces_.empty()) {
        Uint32 now = SDL_GetTicks();
        Uint32 elapsed = now - lastFrameTime_;

        if (elapsed >= (Uint32)frameDelay_) {
            size_t advance = (size_t)(elapsed / (Uint32)frameDelay_);
            currentFrame_ = (currentFrame_ + advance) % animatedSurfaces_.size();
            lastFrameTime_ = now - (elapsed % (Uint32)frameDelay_);

            SDL_Surface* surf = animatedSurfaces_[currentFrame_];
            if (surf && surf->pixels) {
                SDL_UpdateTexture(animatedTexture_, nullptr, surf->pixels, surf->pitch);
            }
        }
    }

    SDL_Texture* target = (animatedTexture_ && frameDelay_ > 0) ? animatedTexture_ : texture_;
    if (!target) return;

    // IMPORTANT: set blend mode right before draw so shared cached textures can be used with different modes.
    SDL_SetTextureBlendMode(target, baseViewInfo.Additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);

    SDL::renderCopyF(
        target,
        baseViewInfo.Alpha,
        nullptr,
        &rect,
        baseViewInfo,
        page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
        page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
}

// -------------------- teardown --------------------

void Image::freeGraphicsMemory() {
    Component::freeGraphicsMemory();

    // Animated streaming texture is ALWAYS per-instance.
    if (animatedTexture_) {
        SDL_DestroyTexture(animatedTexture_);
        animatedTexture_ = nullptr;
    }

    // Static texture: destroy only if instance-owned.
    if (texture_) {
        if (!isUsingCachedStaticTexture_) {
            SDL_DestroyTexture(texture_);
        }
        texture_ = nullptr;
    }

    // Animated surfaces: free only if instance-owned.
    if (!animatedSurfaces_.empty()) {
        if (!isUsingCachedSurfaces_) {
            for (auto* s : animatedSurfaces_) {
                if (s) SDL_DestroySurface(s);
            }
        }
        animatedSurfaces_.clear();
    }

    isUsingCachedStaticTexture_ = false;
    isUsingCachedSurfaces_ = false;
    resetAnimationState();
}

std::string_view Image::filePath() {
    return file_;
}

void Image::cleanupTextureCache() {
    // Single-threaded: no locking needed.
    for (auto& [key, entry] : textureCache_) {
        (void)key;

        if (entry.texture) {
            SDL_DestroyTexture(entry.texture);
            entry.texture = nullptr;
        }

        for (auto* s : entry.animatedSurfaces) {
            if (s) SDL_DestroySurface(s);
        }
        entry.animatedSurfaces.clear();
        entry.frameDelay = 0;
    }

    textureCache_.clear();
}