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
	// --- complete pending retarget on the main thread ---
	if (videoInst_ && pendingRetarget_) {
		if (auto* gst = dynamic_cast<GStreamerVideo*>(videoInst_.get())) {
			if (gst->consumeBecameNone()) {
				instanceReady_ = videoInst_->play(videoFile_); // preroll to PAUSED
				pendingRetarget_ = false;
			}
		}
		else {
			pendingRetarget_ = false; // fail-safe
		}
	}

	if (!videoInst_ || !currentPage_ || !instanceReady_) {
		return Component::update(dt);
	}

	if (!videoInst_->isPipelineReady()) {
		return Component::update(dt);
	}

	if (videoInst_->hasError()) {
		LOG_WARNING("VideoComponent",
			"Update: GStreamerVideo instance for " + videoFile_ +
			" has an error. Halting further video operations for this component.");
		return Component::update(dt);
	}

	// Dimensions once available
	if (!dimensionsUpdated_) {
		const int w = videoInst_->getWidth();
		const int h = videoInst_->getHeight();
		if (w <= 0 || h <= 0) {
			return Component::update(dt);
		}
		baseViewInfo.ImageWidth = static_cast<float>(w);
		baseViewInfo.ImageHeight = static_cast<float>(h);
		dimensionsUpdated_ = true;
		LOG_DEBUG("VideoComponent",
			"Video dimensions ready: " + std::to_string(w) + "x" + std::to_string(h) +
			" for " + videoFile_);
	}

	// Volume
	videoInst_->setVolume(baseViewInfo.Volume);
	if (!currentPage_->isMenuScrolling()) {
		videoInst_->volumeUpdate();
	}

	// Snapshot state
	auto actual = videoInst_->getActualState();
	auto target = videoInst_->getTargetState();
	bool inFlight = (target != actual);

	// Hard rule: if game launched on primary monitor -> pause and return
	if (currentPage_->getIsLaunched() && baseViewInfo.Monitor == 0) {
		if (actual != IVideo::VideoState::Paused &&
			target != IVideo::VideoState::Paused) {
			videoInst_->pause();
		}
		return Component::update(dt);
	}

	// Visibility
	const bool visibleNow = (baseViewInfo.Alpha > 0.0f);
	if (visibleNow) {
		hasBeenOnScreen_ = true;
	}

	// ---------------- Desired state ----------------
	IVideo::VideoState desired = IVideo::VideoState::None;

	// Auto-play when visible (original behavior)
	if (visibleNow) {
		desired = IVideo::VideoState::Playing;
	}

	// Pause-on-scroll: pause only on the HIDE edge
	const bool hideEdge = (baseViewInfo.PauseOnScroll && !visibleNow && wasVisible_);
	if (hideEdge) {
		desired = IVideo::VideoState::Paused;
	}

	// If Restart is requested while hidden and PauseOnScroll is enabled:
	// arm a "restart while paused" and do NOT resume offscreen.
	// (We allow it both on the hide edge and while already hidden.)
	if (baseViewInfo.PauseOnScroll && !visibleNow && baseViewInfo.Restart && hasBeenOnScreen_) {
		restartOnHide_ = true;
		pendingRestart_ = false; // cancel any "resume then seek" plan
	}

	// Apply at most one transition, only if not in-flight
	// Refresh inFlight just before applying (target may have changed elsewhere)
	actual = videoInst_->getActualState();
	target = videoInst_->getTargetState();
	inFlight = (target != actual);

	if (!inFlight && desired != IVideo::VideoState::None) {
		if (desired == IVideo::VideoState::Playing && actual != IVideo::VideoState::Playing) {
			videoInst_->resume();
			LOG_DEBUG("VideoComponent", "Resume -> " + videoFile_);
		}
		else if (desired == IVideo::VideoState::Paused && actual != IVideo::VideoState::Paused) {
			videoInst_->pause();
			LOG_DEBUG("VideoComponent", "Pause -> " + videoFile_);
		}
	}


	// Mode 1: complete "restart while paused" once PAUSED and not in-flight
	if (restartOnHide_) {
		const auto a = videoInst_->getActualState();
		const auto t = videoInst_->getTargetState();
		if (a == IVideo::VideoState::Paused && t == IVideo::VideoState::Paused) {

			videoInst_->restart(); // seek to 0 while PAUSED
			LOG_DEBUG("VideoComponent", "Restart-on-hide: seek to start (paused) for " + videoFile_);

			baseViewInfo.Restart = false;
			restartOnHide_ = false;
		}
	}

	// Mode 2: normal restart (only when NOT hidden under PauseOnScroll)
	if (baseViewInfo.Restart && hasBeenOnScreen_ &&
		!(baseViewInfo.PauseOnScroll && !visibleNow)) {

		const auto a = videoInst_->getActualState();
		const auto t = videoInst_->getTargetState();
		const bool flight = (t != a);

		if (!flight) {
			if (a == IVideo::VideoState::Paused) {
				videoInst_->resume();      // get to PLAYING first
				pendingRestart_ = true;    // perform seek once PLAYING
				LOG_DEBUG("VideoComponent", "Deferred restart: resuming first for " + videoFile_);
			}
			else if (a == IVideo::VideoState::Playing) {
				videoInst_->restart();
				LOG_DEBUG("VideoComponent", "Seeking to beginning of " + Utils::getFileName(videoFile_));
				baseViewInfo.Restart = false;
				pendingRestart_ = false;
			}
			else {
				pendingRestart_ = true; // READY/transitioning: defer
			}
		}
		else {
			pendingRestart_ = true;     // defer while transitioning
		}
	}

	// Complete deferred normal restart once PLAYING & not in-flight
	if (pendingRestart_) {
		const auto a = videoInst_->getActualState();
		const auto t = videoInst_->getTargetState();
		const bool flight = (t != a);

		if (!flight && a == IVideo::VideoState::Playing) {
			videoInst_->restart();
			LOG_DEBUG("VideoComponent", "Post-transition seek to start: " + Utils::getFileName(videoFile_));
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