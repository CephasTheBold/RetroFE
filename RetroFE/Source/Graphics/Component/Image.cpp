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
	// If we are already loading or ready, don't restart unless recycleAsImage set us to Unloaded
	if (status_ != LoadStatus::Unloaded) return;
	if (useTextureCaching_) ensureCacheReserved();

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
		{
			std::lock_guard<std::mutex> lock(g_ImageCacheMutex);
			loadingTasks_.erase(path);
		}
		return res;
		}).share();

	loadingTasks_[path] = loadTask_;
	return true;
}

void Image::finalizeLoad() {
	std::string path = currentLoadingPath_;

	// 1. Check Cache first
	{
		std::lock_guard<std::mutex> lock(g_ImageCacheMutex);
		if (useTextureCaching_ && loadFromCache(path)) {
			status_ = LoadStatus::Ready;
			return;
		}
	}

	AsyncLoadResult res = loadTask_.get();
	loadTask_ = {};

	if (!res.success) {
		if (path == file_ && !altFile_.empty()) {
			status_ = LoadStatus::Unloaded;
			startAsyncLoad(altFile_);
			return;
		}
		status_ = LoadStatus::Error;
		return;
	}

	// 2. Prepare the NEW assets
	SDL_Renderer* renderer = SDL::getRenderer(baseViewInfo.Monitor);
	SDL_Texture* newTexture = nullptr;
	std::vector<SharedSurface> newAnimatedSurfaces;
	std::vector<int> newFrameDelays;

	if (res.staticSurface) {
		newTexture = SDL_CreateTextureFromSurface(renderer, res.staticSurface.get());
	}
	else if (!res.animatedSurfaces.empty()) {
		newAnimatedSurfaces = res.animatedSurfaces;
		newFrameDelays = res.frameDelays;
	}

	// 3. THE SWAP: Only clear old assets if we actually have new ones to show
	if (newTexture || !newAnimatedSurfaces.empty()) {
		// Cleanup old local assets before overwriting pointers (Prevent Leaks)
		if (animatedTexture_) {
			SDL_DestroyTexture(animatedTexture_);
			animatedTexture_ = nullptr;
		}
		if (texture_ && !isUsingCachedStaticTexture_) {
			SDL_DestroyTexture(texture_);
		}

		// Assign new assets
		texture_ = newTexture;
		animatedSurfaces_ = newAnimatedSurfaces;
		frameDelays_ = newFrameDelays;

		baseViewInfo.ImageWidth = (float)res.w;
		baseViewInfo.ImageHeight = (float)res.h;
		status_ = LoadStatus::Ready;

		if (!animatedSurfaces_.empty()) {
			createAnimatedStreamingTexture(res.w, res.h);
		}

		if (useTextureCaching_) {
			std::lock_guard<std::mutex> lock(g_ImageCacheMutex);
			auto key = pathCache_.getKey(path, baseViewInfo.Monitor);
			CachedImage ci;
			ci.texture = texture_;
			ci.animatedSurfaces = animatedSurfaces_;
			ci.frameDelays = frameDelays_;
			ci.w = res.w;
			ci.h = res.h;
			textureCache_[key] = ci;
			isUsingCachedStaticTexture_ = (texture_ != nullptr);
			isUsingCachedSurfaces_ = !animatedSurfaces_.empty();
		}
		else {
			isUsingCachedStaticTexture_ = false; // Instance owns this now
			isUsingCachedSurfaces_ = false;
		}
		primeAnimatedTextureIfNeeded();
		resetAnimationState();
	}
	else {
		status_ = LoadStatus::Error;
	}
}

// -------------------- Render Logic --------------------
bool Image::loadFromCache(const std::string& filePath) {
	// This helper assumes a mutex lock is already held by the caller
	auto it = textureCache_.find(pathCache_.getKey(filePath, baseViewInfo.Monitor));
	if (it == textureCache_.end()) return false;

	const CachedImage& ci = it->second;

	// --- FIX THE LEAK ---
	// 1. Destroy the current static texture if it's not a cached reference
	if (texture_ && !isUsingCachedStaticTexture_) {
		SDL_DestroyTexture(texture_);
	}
	texture_ = nullptr;
	isUsingCachedStaticTexture_ = false;

	// 2. Destroy the animated streaming texture if it exists
	// (Animated streaming textures are always instance-local, never shared)
	if (animatedTexture_) {
		SDL_DestroyTexture(animatedTexture_);
		animatedTexture_ = nullptr;
	}
	// --------------------

	// Restore dimensions instantly
	baseViewInfo.ImageWidth = (float)ci.w;
	baseViewInfo.ImageHeight = (float)ci.h;

	if (ci.texture) {
		texture_ = ci.texture;
		isUsingCachedStaticTexture_ = true;
	}
	else if (!ci.animatedSurfaces.empty()) {
		animatedSurfaces_ = ci.animatedSurfaces;
		frameDelays_ = ci.frameDelays;
		isUsingCachedSurfaces_ = true;

		// createAnimatedStreamingTexture will allocate a NEW texture
		createAnimatedStreamingTexture((int)baseViewInfo.ImageWidth, (int)baseViewInfo.ImageHeight);
	}

	if (texture_ || animatedTexture_) {
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
	}

	if (status_ == LoadStatus::Error || (status_ == LoadStatus::Unloaded && !texture_ && !animatedTexture_)) {
		return;
	}

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
	if (animatedTexture_) SDL_DestroyTexture(animatedTexture_);
	if (texture_ && !isUsingCachedStaticTexture_) SDL_DestroyTexture(texture_);

	texture_ = nullptr;
	animatedTexture_ = nullptr;
	animatedSurfaces_.clear();
	frameDelays_.clear();
	isUsingCachedStaticTexture_ = isUsingCachedSurfaces_ = false;
	status_ = LoadStatus::Unloaded;

	loadTask_ = {};

	// Reset our local path so we don't accidentally check it later
	currentLoadingPath_.clear();
	resetAnimationState();
}

void Image::resetAnimationState() {
	currentFrame_ = 0;
	animationStartTime_ = 0;
	lastRenderedFrame_ = std::numeric_limits<size_t>::max();
}

bool Image::createAnimatedStreamingTexture(int width, int height) {
	if (width <= 0 || height <= 0) return false;

	// Ensure we aren't leaking a previous streaming texture
	if (animatedTexture_) {
		SDL_DestroyTexture(animatedTexture_);
	}

	animatedTexture_ = SDL_CreateTexture(SDL::getRenderer(baseViewInfo.Monitor),
		SDL_PIXELFORMAT_RGBA32,
		SDL_TEXTUREACCESS_STREAMING,
		width, height);
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
	pathCache_.fullPaths_.clear();
}

bool Image::recycleAsImage(const std::string& newFilePath, const std::string& newAltPath) {
	if (newFilePath.empty() && newAltPath.empty()) return false;

	// 1. Skip if the file is already the one we are showing
	if (file_ == newFilePath && altFile_ == newAltPath && status_ != LoadStatus::Error) {
		return true;
	}

	// 2. SOFT RESET: Update the paths
	file_ = newFilePath;
	altFile_ = newAltPath;

	// 3. FORCE THE LOAD: Bypass the status guard in allocateGraphicsMemory
	status_ = LoadStatus::Unloaded;

	// 4. TRIGGER ASYNC: This will now move status to 'Loading'
	allocateGraphicsMemory();

	// NOTE: We do NOT zero out ImageWidth/Height and do NOT call freeGraphicsMemory().
	// This allows the draw() function to keep using the existing texture_ 
	// and dimensions until finalizeLoad() swaps them for the new ones.

	return true;
}

std::string_view Image::filePath() { return file_; }