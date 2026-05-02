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
#include <memory>
#include <mutex>

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
	bool isPlaying() override;
	void setVolume(float volume) override;
	VideoDim getDimensions() override;
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

	// --- Carousel / crossfade helpers ---
	// Prime a slot in PAUSED with optional audio decode.
	// enableAudio=false skips audio decode, reducing preroll latency for side/offscreen slots.
	bool prime(const std::string& file, bool enableAudio = false);

	// Signal center-mode transitions.
	// isCenterMode=true  (side→center): re-enables audio pipeline and allows volume to ramp.
	// isCenterMode=false (center→side): immediately mutes audio output.
	void setCenterMode(bool isCenterMode);

	// Move pipeline to GST_STATE_READY and drain appsinks. Use for far-away slots
	// that are no longer adjacent and should not consume decode/memory resources.
	bool hibernate();


	bool hasFinishedLoops() const;


	bool hasError() const override {
		return hasError_.load(std::memory_order_acquire);
	}
	IVideo::VideoState getTargetState() const override { return targetState_.load(std::memory_order_acquire); }
	IVideo::VideoState getActualState() const override { return actualState_.load(std::memory_order_acquire); }
	bool isPipelineReady() const override { return pipeLineReady_.load(std::memory_order_acquire); }
	static void enablePlugin(const std::string& pluginName);
	static void disablePlugin(const std::string& pluginName);
	
private:
	struct AsyncState {
		std::mutex mutex;
		std::atomic<bool> alive{ true };
	};
	std::shared_ptr<AsyncState> asyncState_ = std::make_shared<AsyncState>();

	// --- Callback context to avoid UAF in GStreamer/GLib callbacks ---
	struct CallbackCtx {
		// GLib atomic refcount so callbacks can hold refs safely across threads
		grefcount ref{};

		// Shared lifetime gate (refcounted independently of GStreamerVideo)
		std::shared_ptr<AsyncState> state;

		// Raw pointer to the instance; set to nullptr during teardown to detach safely
		std::atomic<GStreamerVideo*> self{ nullptr };
	};

	// One ctx per pipeline/appsink set. Owned by this instance while pipeline_ exists.
	CallbackCtx* cbCtx_ = nullptr;

	// ref/unref helpers
	static CallbackCtx* cbCtxRef(CallbackCtx* c);
	static void cbCtxUnref(gpointer data);

	// === Thread-shared atomics ===
	std::atomic<uint64_t> currentPlaySessionId_{ 0 };
	static std::atomic<uint64_t> nextUniquePlaySessionId_;
	std::atomic<bool> hasError_{ false };              // Set by pad probe, read main

	// === Main-thread only ===
	std::atomic<IVideo::VideoState> targetState_{ IVideo::VideoState::None };
	std::atomic<IVideo::VideoState> actualState_{ IVideo::VideoState::None };

	std::atomic<VideoDim> dimensions_{};

	std::atomic<int> playCount_{ 0 };
	std::atomic<int> numLoops_{ 0 };
	std::string currentFile_{};
	float volume_{ 0.0f };
	bool audioEnabled_{ true };  // tracks whether the current pipeline was built with audio
	
	// Tracked allocation state to avoid redundant SDL_QueryTexture calls
	int allocatedWidth_{ 0 };
	int allocatedHeight_{ 0 };
	SDL_PixelFormatEnum allocatedFormat_{ SDL_PIXELFORMAT_UNKNOWN };
	bool isTextureReady_{ false };
	int monitor_;
	bool softOverlay_;
	int perspectiveCorners_[8]{ 0 };
	bool hasPerspective_{ false };
	std::atomic<bool> pipeLineReady_{ false };


	// === GStreamer and SDL resource pointers ===
	GstElement* pipeline_{ nullptr };   // top-level pipeline
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
	static GstPadProbeReturn padProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
	static void initializePlugins();
	void createSdlTexture();
	void destroyTextures(); // Centralized cleanup
	bool updateTextureFromFrameIYUV(SDL_Texture*, GstVideoFrame*) const;
	bool updateTextureFromFrameNV12(SDL_Texture*, GstVideoFrame*) const;
	bool updateTextureFromFrameRGBA(SDL_Texture*, GstVideoFrame*) const;
	std::string generateDotFileName(const std::string& prefix, const std::string& videoFilePath) const;

	std::atomic<bool> loopsFinished_{ false }; // Indicates when all loops have finished	
	std::atomic<bool> hasVideoStream_{ true };  // Assume true until proven otherwise

	// Audio bus integration
	AudioBus::SourceId videoSourceId_{ 0 };   // ID of this video’s source in AudioBus
	std::shared_ptr<AudioBus::Handle> audioHandle_; // NEW: cached handle for lock-free hot path
	GstElement* audioSink_{ nullptr };        // GStreamer appsink for audio
};

