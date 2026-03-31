/* This file is part of RetroFE.

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
#pragma once

#include "../Database/Configuration.h"
#include "../SDL.h"
#include "../Utility/Utils.h"
#include "../Utility/Log.h"
#include "../Sound/AudioBus.h" 
#include "IVideo.h"
#include <atomic>
#include <string>
#include <vector>
#include <functional>

extern "C" {
#if (__APPLE__)
#if __has_include(<gstreamer-1.0/gst/gst.h>)
#include <gstreamer-1.0/gst/gst.h>
#include <gstreamer-1.0/gst/video/video.h>
#include <gstreamer-1.0/gst/app/gstappsink.h>
#elif __has_include(<GStreamer/gst/gst.h>)
#include <GStreamer/gst/gst.h>
#include <GStreamer/gst/video/video.h>
#include <GStreamer/gst/app/gstappsink.h>
#else
#error "Cannot find Gstreamer headers"
#endif
#else
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#endif
}

class GStreamerVideo final : public IVideo {

public:
	explicit GStreamerVideo(int monitor);
	GStreamerVideo(const GStreamerVideo&) = delete;
	GStreamerVideo& operator=(const GStreamerVideo&) = delete;
	~GStreamerVideo() override;

	// --- Interface methods ---
	bool initialize() override;
	bool deInitialize() override;
	bool unload();
	bool createPipelineIfNeeded();
	bool play(const std::string& file) override;
	bool stop() override;
	SDL_Texture* getTexture() const override;
	void volumeUpdate() override;
	void draw() override;
	void setNumLoops(int n);
	int getHeight() override;
	int getWidth() override;
	bool isPlaying() override;
	void setVolume(float volume) override;
	void skipForward() override;
	void skipBackward() override;
	void skipForwardp() override;
	void skipBackwardp() override;
	void pause() override;
	void resume() override;
	void restart() override;
	void loop();
	unsigned long long getCurrent() override;
	unsigned long long getDuration() override;
	bool isPaused() override;
	void setSoftOverlay(bool value);
	void setPerspectiveCorners(const int* corners);
	bool hasVideoStream() const { return hasVideoStream_; }


	bool hasFinishedLoops() const;
	void setOnReadyCallback(std::function<void(IVideo*)> cb) override;


	bool hasError() const override {
		return hasError_.load(std::memory_order_acquire);
	}
	IVideo::VideoState getTargetState() const override { return targetState_.load(std::memory_order_acquire); }
	IVideo::VideoState getActualState() const override { return actualState_.load(std::memory_order_acquire); }
	bool isPipelineReady() const override { return pipeLineReady_.load(std::memory_order_acquire); }
	static void enablePlugin(const std::string& pluginName);
	static void disablePlugin(const std::string& pluginName);
	
	std::atomic<bool> becameNone_{ false };

	bool consumeBecameNone() {
		return becameNone_.exchange(false, std::memory_order_acq_rel);
	}
	void armOnBecameNone() {
		becameNone_.store(false, std::memory_order_release);
		notifyOnNone_.store(true, std::memory_order_release);
	}
	void disarmOnBecameNone() {
		notifyOnNone_.store(false, std::memory_order_release);
		becameNone_.store(false, std::memory_order_release);
	}

private:
	// === Thread-shared atomics ===
	std::atomic<uint64_t> currentPlaySessionId_{ 0 };
	static std::atomic<uint64_t> nextUniquePlaySessionId_;
	std::atomic<bool> hasError_{ false };              // Set by pad probe, read main

	// === Main-thread only ===
	std::atomic<IVideo::VideoState> targetState_{ IVideo::VideoState::None };
	std::atomic<IVideo::VideoState> actualState_{ IVideo::VideoState::None };

	int width_{ -1 };
	int height_{ -1 };
	int playCount_{ 0 };
	std::string currentFile_{};
	int numLoops_{ 0 };
	float volume_{ 0.0f };
	
	// Tracked allocation state to avoid redundant SDL_QueryTexture calls
	int allocatedWidth_{ 0 };
	int allocatedHeight_{ 0 };
	SDL_PixelFormatEnum allocatedFormat_{ SDL_PIXELFORMAT_UNKNOWN };
	int allocatedRingCount_{ 0 };
	
	int monitor_;
	bool softOverlay_;
	int perspectiveCorners_[8]{ 0 };
	bool hasPerspective_{ false };
	std::atomic<bool> pipeLineReady_{ false };


	// === GStreamer and SDL resource pointers ===
	GstElement* pipeline_{ nullptr };   // top-level pipeline
	GstElement* playbin_{ nullptr };
	GstElement* videoSink_{ nullptr };
	GstElement* perspective_{ nullptr };
	int lastPerspectiveW_{ -1 };
	int lastPerspectiveH_{ -1 };
	SDL_Texture* texture_{ nullptr };
	SDL_PixelFormatEnum sdlFormat_{ SDL_PIXELFORMAT_UNKNOWN };
	guint elementSetupHandlerId_{ 0 };
	guint busWatchId_{ 0 };
	guint padProbeId_{ 0 };
	GValueArray* gva_{ nullptr };
	GValueArray* perspective_gva_{ nullptr };
	std::atomic<GstSample*> stagedSample_{ nullptr };

	// === Static/shared ===
	static bool initialized_;
	static bool pluginsInitialized_;


	// === Internal helpers ===
	struct SessionCtx {
		GStreamerVideo* self{ nullptr };
		uint64_t session{ 0 };
	};
	GstAppSinkCallbacks audioCbs_{};
	GstAppSinkCallbacks videoCbs_{};
	SessionCtx* audioCtx_ = nullptr;    // we own and free explicitly
	SessionCtx* videoCtx_ = nullptr;

	static constexpr int kVideoRing = 3; // set to 3 if you still see the odd hitch
	SDL_Texture* videoTexRing_[3]{ nullptr, nullptr, nullptr };
	int          videoRingCount_{ kVideoRing };
	int          videoWriteIdx_{ 0 };
	int          videoDrawIdx_{ -1 };  // last slot published for rendering

	// Small helper so every callback early-outs the same way
	inline bool isCurrentSession(uint64_t s) const {
		return s == currentPlaySessionId_.load(std::memory_order_acquire) &&
			targetState_.load(std::memory_order_acquire) != IVideo::VideoState::None;
	}

	static gboolean busCallback(GstBus* bus, GstMessage* msg, gpointer user_data);
	static void elementSetupCallback(GstElement* playbin, GstElement* element, gpointer data);
	static GstFlowReturn on_new_preroll(GstAppSink* sink, gpointer user_data);
	static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
	static GstFlowReturn on_audio_new_sample(GstAppSink* sink, gpointer user_data);
	void setupCallbacksForSession(uint64_t sessionId);
	static GstPadProbeReturn padProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
	static void initializePlugins();
	void createSdlTexture();
	void destroyTextures(); // Centralized cleanup
	bool updateTextureFromFrameIYUV(SDL_Texture*, GstVideoFrame*) const;
	bool updateTextureFromFrameNV12(SDL_Texture*, GstVideoFrame*) const;
	bool updateTextureFromFrameRGBA(SDL_Texture*, GstVideoFrame*) const;
	std::string generateDotFileName(const std::string& prefix, const std::string& videoFilePath) const;

	std::atomic<bool> notifyOnNone_{ false };
	std::atomic<bool> unloading_{ false };
	std::atomic<bool> loopsFinished_{ false }; // Indicates when all loops have finished	
	std::atomic<bool> hasVideoStream_{ true };  // Assume true until proven otherwise

	// Audio bus integration
	AudioBus::SourceId videoSourceId_{ 0 };   // ID of this video’s source in AudioBus
	GstElement* audioSink_{ nullptr };        // GStreamer appsink for audio
	std::atomic<bool> audioRun_{ false };     // Control flag for the feeder loop

	std::function<void(IVideo*)> onReadyCallback_;
};

