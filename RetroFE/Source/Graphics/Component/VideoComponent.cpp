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
    // 1. --- Initialization & Retargeting ---
    if (videoInst_ && pendingRetarget_) {
        instanceReady_ = videoInst_->play(videoFile_);
        pendingRetarget_ = false;
        LOG_DEBUG("VideoComponent", "[Init] Prerolling to PAUSED: " + videoFile_);
    }

    if (!videoInst_ || !currentPage_ || !instanceReady_ || !videoInst_->isPipelineReady() || videoInst_->hasError()) {
        return Component::update(dt);
    }

    // 2. --- Dimension & Volume Handling ---
    const bool audioOnly = !hasVideoStream();
    if (!dimensionsUpdated_) {
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
            return Component::update(dt);
        }
    }

    videoInst_->setVolume(baseViewInfo.Volume);
    videoInst_->volumeUpdate();

    // 3. --- Visibility & State Tracking ---
    const float screenW = static_cast<float>(currentPage_->getLayoutWidthByMonitor(baseViewInfo.Monitor));
    const float screenH = static_cast<float>(currentPage_->getLayoutHeightByMonitor(baseViewInfo.Monitor));

    bool physicallyOnScreen = (baseViewInfo.XRelativeToOrigin() + baseViewInfo.ScaledWidth() > 0.0f) &&
        (baseViewInfo.XRelativeToOrigin() < screenW) &&
        (baseViewInfo.YRelativeToOrigin() + baseViewInfo.ScaledHeight() > 0.0f) &&
        (baseViewInfo.YRelativeToOrigin() < screenH);

    const bool visibleNow = (baseViewInfo.Alpha > 0.0f) && physicallyOnScreen;
    auto actual = videoInst_->getActualState();
    auto target = videoInst_->getTargetState();

    if (actual == IVideo::VideoState::Playing && !hasBeenOnScreen_) {
        hasBeenOnScreen_ = true;
        LOG_DEBUG("VideoComponent", "[Lifecycle] Playback confirmed: " + videoFile_);

        if (listId_ == -1 && numLoops_ > 0) {
            setAnimationDoneRemove(true);
        }
    }

    // 4. --- Determine Desired State ---
    IVideo::VideoState desired = IVideo::VideoState::None;

    // --- THE FIX: Integrated Priority Launch Lock ---
    // If a game is active on Monitor 0, force the state to Paused.
    // Removed 'listId_ != -1' check so layout videos/effects also pause.
    if (currentPage_->getIsLaunched() && baseViewInfo.Monitor == 0) {
        desired = IVideo::VideoState::Paused;
    }
    else if (visibleNow && !hasFinishedLoops()) {
        // Fast-scrolling logic applies to menu list items
        if (listId_ != -1 && currentPage_->isMenuFastScrolling()) {
            desired = (actual == IVideo::VideoState::Playing || target == IVideo::VideoState::Playing)
                ? IVideo::VideoState::Playing : IVideo::VideoState::Paused;
        }
        else {
            desired = IVideo::VideoState::Playing;
        }
    }
    else {
        desired = IVideo::VideoState::Paused;
    }

    // 5. --- Apply State Transitions ---
    if (target == actual && desired != IVideo::VideoState::None) {
        if (desired == IVideo::VideoState::Playing && actual != IVideo::VideoState::Playing) {
            videoInst_->resume();
            LOG_DEBUG("VideoComponent", "[Transition] Resume: " + videoFile_);
        }
        else if (desired == IVideo::VideoState::Paused && actual != IVideo::VideoState::Paused) {
            videoInst_->pause();
            LOG_DEBUG("VideoComponent", "[Transition] Pause: " + videoFile_);
        }
    }

    // 6. --- Restart Logic ---
    if (baseViewInfo.Restart && !hasBeenOnScreen_) {
        baseViewInfo.Restart = false;
    }

    if (!visibleNow && wasVisible_ && baseViewInfo.Restart && hasBeenOnScreen_) {
        restartOnHide_ = true;
        baseViewInfo.Restart = false;
        pendingRestart_ = false;
    }

    if ((restartOnHide_ || baseViewInfo.Restart) && hasBeenOnScreen_) {
        actual = videoInst_->getActualState();
        target = videoInst_->getTargetState();
        if (target == actual && (actual == IVideo::VideoState::Playing || actual == IVideo::VideoState::Paused)) {
            videoInst_->restart();
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
        if (target == actual && (actual == IVideo::VideoState::Playing || actual == IVideo::VideoState::Paused)) {
            videoInst_->restart();
            baseViewInfo.Restart = false;
            pendingRestart_ = false;
        }
    }

    wasVisible_ = visibleNow;

    // 7. --- Terminal Lifecycle ---
    bool animationsDone = Component::update(dt);

    if (!hasBeenOnScreen_) {
        return false;
    }

    if (listId_ == -1 && numLoops_ > 0) {
        if (hasFinishedLoops() && animationsDone) {
            return true;
        }
        if (animationsDone && !visibleNow) {
            return true;
        }
        return false;
    }

    return animationsDone;
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