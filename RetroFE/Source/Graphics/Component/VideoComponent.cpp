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

#include "VideoComponent.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <memory>

#include "../../Video/GStreamerVideo.h"
#include "../../Graphics/ViewInfo.h"
#include "../../SDL.h"
#include "../../Utility/Log.h"
#include "../../Utility/ThreadPool.h"
#include "../../Utility/Utils.h"
#include "../../Video/IVideo.h"
#include "../../Video/VideoFactory.h"
#include "../Page.h"
#ifdef __APPLE__
#include "SDL2/SDL_rect.h"
#include "SDL2/SDL_render.h"
#else
#include "SDL_rect.h"
#include "SDL_render.h"
#endif
#include <gst/video/video.h>
#include "../../Video/VideoPool.h"

VideoComponent::VideoComponent(Page& p, const std::string& videoFile, int monitor, int numLoops, bool softOverlay, int listId, const int* perspectiveCorners)
	: Component(p), videoFile_(videoFile), softOverlay_(softOverlay), numLoops_(numLoops), monitor_(monitor), listId_(listId), currentPage_(&p) {
	if (perspectiveCorners) {
		std::copy(perspectiveCorners, perspectiveCorners + 8, perspectiveCorners_);
		hasPerspective_ = true;
	}
}

VideoComponent::~VideoComponent() {
	LOG_DEBUG("VideoComponent", "Destroying VideoComponent for file: " + videoFile_);
	VideoComponent::freeGraphicsMemory();
}

bool VideoComponent::update(float dt) {

	// 1. --- complete pending retarget on the main thread ---
	if (videoInst_ && pendingRetarget_) {
		// We no longer need to wait for consumeBecameNone(). 
		// GStreamerVideo::play() handles its own internal serialization natively!
		instanceReady_ = videoInst_->play(videoFile_);
		pendingRetarget_ = false;
		LOG_DEBUG("VideoComponent", "[Init] Prerolling to PAUSED: " + videoFile_);
	}

	if (!videoInst_ || !currentPage_ || !instanceReady_ || !videoInst_->isPipelineReady() || videoInst_->hasError()) {
		return Component::update(dt);
	}

	// 2. --- Dimensions & Universal Volume ---
	const bool audioOnly = !hasVideoStream();
	if (!dimensionsUpdated_) {
		// One atomic load to get the synchronized pair
		const auto dims = videoInst_->getDimensions();

		if (audioOnly || (dims.w > 0 && dims.h > 0)) {
			if (!audioOnly) {
				baseViewInfo.ImageWidth = static_cast<float>(dims.w);
				baseViewInfo.ImageHeight = static_cast<float>(dims.h);
			}
			dimensionsUpdated_ = true;
			LOG_DEBUG("VideoComponent", "Dimensions locked: " + std::to_string(dims.w) + "x" + std::to_string(dims.h));
		}
		else {
			return Component::update(dt); // Still waiting for GStreamer to report size
		}
	}

	videoInst_->setVolume(baseViewInfo.Volume);
	videoInst_->volumeUpdate();

	const bool visibleNow = (baseViewInfo.Alpha > 0.0f);
	auto actual = videoInst_->getActualState();
	auto target = videoInst_->getTargetState();

	// 3. --- Standalone Video Logic (listId_ == -1) ---
	if (listId_ == -1) {
		if (visibleNow) {
			if (actual != IVideo::VideoState::Playing && target != IVideo::VideoState::Playing) {
				videoInst_->resume();
				LOG_DEBUG("VideoComponent", "[Standalone] Resume (visible): " + videoFile_);
			}
		}
		else {
			if (actual != IVideo::VideoState::Paused && target != IVideo::VideoState::Paused) {
				videoInst_->pause();
				LOG_DEBUG("VideoComponent", "[Standalone] Pause (hidden): " + videoFile_);
			}
		}
		return Component::update(dt);
	}

	// 4. --- List Item Logic (listId_ != -1) ---

	// Set 'Played' flag only when it actually hits the Playing state
	if (actual == IVideo::VideoState::Playing && !hasBeenOnScreen_) {
		hasBeenOnScreen_ = true;
		LOG_DEBUG("VideoComponent", "[State] First Playback confirmed: " + videoFile_);
	}

	// Launch Lock
	if (currentPage_->getIsLaunched() && baseViewInfo.Monitor == 0) {
		if (actual != IVideo::VideoState::Paused && target != IVideo::VideoState::Paused) {
			videoInst_->pause();
			LOG_DEBUG("VideoComponent", "[Lock] Pausing for Game Launch: " + videoFile_);
		}
		return Component::update(dt);
	}

	// ---------------- Desired state ----------------
	IVideo::VideoState desired = IVideo::VideoState::None;

	if (visibleNow && !hasFinishedLoops()) {
		if (currentPage_->isMenuFastScrolling()) {
			// Rule: Keep playing if already active, stay paused if new
			if (actual == IVideo::VideoState::Playing || target == IVideo::VideoState::Playing) {
				desired = IVideo::VideoState::Playing;
			}
			else {
				desired = IVideo::VideoState::Paused;
			}
		}
		else {
			desired = IVideo::VideoState::Playing;
		}
	}
	else {
		desired = IVideo::VideoState::Paused;
	}

	// Apply transitions
	bool inFlight = (target != actual);
	if (!inFlight && desired != IVideo::VideoState::None) {
		if (desired == IVideo::VideoState::Playing && actual != IVideo::VideoState::Playing) {
			videoInst_->resume();
			LOG_DEBUG("VideoComponent", "[Transition] Resume: " + videoFile_);
		}
		else if (desired == IVideo::VideoState::Paused && actual != IVideo::VideoState::Paused) {
			videoInst_->pause();
			LOG_DEBUG("VideoComponent", "[Transition] Pause: " + videoFile_);
		}
	}

	// ---------------- Restart Logic ----------------

	// Optimization: Clear restart flag if the video never actually played content
	if (baseViewInfo.Restart && !hasBeenOnScreen_) {
		baseViewInfo.Restart = false;
		LOG_DEBUG("VideoComponent", "[Restart] Skipping - never left 0.0s: " + videoFile_);
	}

	// Mode 1: Restart-on-hide (Alpha 0 edge)
	if (!visibleNow && wasVisible_ && baseViewInfo.Restart && hasBeenOnScreen_) {
		restartOnHide_ = true;
		baseViewInfo.Restart = false;
		pendingRestart_ = false;
		LOG_DEBUG("VideoComponent", "[Restart] Arming Restart-on-Hide: " + videoFile_);
	}

	// Execute direct or deferred seeks
	if ((restartOnHide_ || baseViewInfo.Restart) && hasBeenOnScreen_) {
		actual = videoInst_->getActualState();
		target = videoInst_->getTargetState();
		if (!(target != actual) && (actual == IVideo::VideoState::Playing || actual == IVideo::VideoState::Paused)) {
			videoInst_->restart();
			LOG_DEBUG("VideoComponent", "[Restart] Executing Seek to 0: " + videoFile_);
			baseViewInfo.Restart = false;
			restartOnHide_ = false;
			pendingRestart_ = false;
		}
		else {
			pendingRestart_ = true;
		}
	}

	if (pendingRestart_) {
		actual = videoInst_->getActualState();
		target = videoInst_->getTargetState();
		if (!(target != actual) && (actual == IVideo::VideoState::Playing || actual == IVideo::VideoState::Paused)) {
			videoInst_->restart();
			LOG_DEBUG("VideoComponent", "[Restart] Executing Deferred Seek: " + videoFile_);
			baseViewInfo.Restart = false;
			pendingRestart_ = false;
		}
	}

	wasVisible_ = visibleNow;
	return Component::update(dt);
}

void VideoComponent::allocateGraphicsMemory() {
	Component::allocateGraphicsMemory();
	if (videoInst_) {
		return;
	}
	if (!videoFile_.empty()) {
		videoInst_ = VideoFactory::createVideo(
			monitor_, numLoops_, softOverlay_, listId_,
			hasPerspective_ ? perspectiveCorners_ : nullptr
		);

		if (!videoInst_) {
			LOG_ERROR("VideoComponent", "Failed to create a video instance from the factory.");
		}
		else {
			LOG_DEBUG("VideoComponent", "Issuing play command for: " + videoFile_);
			instanceReady_ = videoInst_->play(videoFile_);
		}
	}
}

std::unique_ptr<IVideo> VideoComponent::extractVideo() {
	instanceReady_ = false;
	return std::move(videoInst_);
}

void VideoComponent::freeGraphicsMemory() {
	Component::freeGraphicsMemory();

	if (!videoInst_) return;  // Already extracted

	if (listId_ != -1) {
		LOG_DEBUG("VideoComponent", "Releasing video to pool: " + videoFile_);
		auto video = std::move(videoInst_);
		VideoPool::releaseVideo(std::move(video), monitor_, listId_);
		return;
	}
	pendingRetarget_ = false;
	LOG_DEBUG("VideoComponent", "Stopping and resetting video: " + videoFile_);
	videoInst_.reset();
}


void VideoComponent::draw() {
	if (!videoInst_ || !currentPage_ || !instanceReady_) {
		return;
	}

	if (videoInst_->isPipelineReady())
		videoInst_->draw();

	if (SDL_Texture* texture = videoInst_->getTexture()) {
		SDL_FRect rect = {
			baseViewInfo.XRelativeToOrigin(), baseViewInfo.YRelativeToOrigin(),
			baseViewInfo.ScaledWidth(), baseViewInfo.ScaledHeight() };

		SDL::renderCopyF(texture, baseViewInfo.Alpha, nullptr, &rect, baseViewInfo,
			page.getLayoutWidthByMonitor(baseViewInfo.Monitor),
			page.getLayoutHeightByMonitor(baseViewInfo.Monitor));
	}
}

std::string_view VideoComponent::filePath() {
	return videoFile_;
}

void VideoComponent::skipForward() {
	if (!videoInst_) {
		return;
	}
	videoInst_->skipForward();
}

void VideoComponent::skipBackward() {
	if (!videoInst_) {
		return;
	}
	videoInst_->skipBackward();
}

void VideoComponent::skipForwardp() {
	if (!videoInst_) {
		return;
	}
	videoInst_->skipForwardp();
}

void VideoComponent::skipBackwardp() {
	if (!videoInst_) {
		return;
	}
	videoInst_->skipBackwardp();
}

void VideoComponent::pause() {
	if (!videoInst_) {
		return;
	}
	videoInst_->pause();
}

void VideoComponent::resume() {
	if (!videoInst_) {
		return;
	}
	videoInst_->resume();
}


void VideoComponent::restart() {
	if (!videoInst_) {
		return;
	}
	videoInst_->restart();
}

unsigned long long VideoComponent::getCurrent() {
	if (!videoInst_) {
		return 0;
	}
	return videoInst_->getCurrent();
}

unsigned long long VideoComponent::getDuration() {
	if (!videoInst_) {
		return 0;
	}
	return videoInst_->getDuration();
}

bool VideoComponent::isPaused() {
	if (!videoInst_) {
		return false;
	}
	return videoInst_->isPaused();
}

bool VideoComponent::isPlaying() {
	if (!videoInst_) {
		return false;
	}
	return videoInst_->isPlaying();
}

bool VideoComponent::hasFinishedLoops() {
	if (!videoInst_) {
		return true;
	}
	return videoInst_->hasFinishedLoops();
}

bool VideoComponent::hasVideoStream() {
	if (!videoInst_) {
		return false;
	}
	if (auto* gst = dynamic_cast<GStreamerVideo*>(videoInst_.get())) {
		return gst->hasVideoStream();
	}
	return true; // assume non-Gst videos have video stream
}