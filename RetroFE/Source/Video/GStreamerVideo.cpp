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
#ifdef WIN32
#define NOMINMAX
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif
#include "GStreamerVideo.h"
#include "GlibLoop.h"
#include "../Database/Configuration.h"
#include "../Graphics/Component/Image.h"
#include "../Graphics/ViewInfo.h"
#include "../SDL.h"
#include "../Utility/Log.h"
#include "../Utility/ThreadPool.h"
#include "../Utility/Utils.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gst/audio/audio.h>
#include <gst/gstdebugutils.h>
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include <array>
#include <algorithm>
#include <utility>
#include <memory>

bool GStreamerVideo::initialized_ = false;
bool GStreamerVideo::pluginsInitialized_ = false;

// Initialize the static session ID generator
std::atomic<uint64_t> GStreamerVideo::nextUniquePlaySessionId_{ 1 };

typedef enum {
	GST_PLAY_FLAG_VIDEO = (1 << 0),
	GST_PLAY_FLAG_AUDIO = (1 << 1),
	GST_PLAY_FLAG_TEXT = (1 << 2),
	GST_PLAY_FLAG_VIS = (1 << 3),
	GST_PLAY_FLAG_SOFT_VOLUME = (1 << 4),
	GST_PLAY_FLAG_NATIVE_AUDIO = (1 << 5),
	GST_PLAY_FLAG_NATIVE_VIDEO = (1 << 6),
	GST_PLAY_FLAG_DOWNLOAD = (1 << 7),
	GST_PLAY_FLAG_BUFFERING = (1 << 8),
	GST_PLAY_FLAG_DEINTERLACE = (1 << 9),
	GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10),
	GST_PLAY_FLAG_FORCE_FILTERS = (1 << 11),
	GST_PLAY_FLAG_FORCE_SW_DECODERS = (1 << 12),
} GstPlayFlags;

static const SDL_BlendMode softOverlayBlendMode = SDL_ComposeCustomBlendMode(
	SDL_BLENDFACTOR_SRC_ALPHA,           // Source color factor: modulates source color by the alpha value set dynamically
	SDL_BLENDFACTOR_ONE,                 // Destination color factor: keep the destination as is
	SDL_BLENDOPERATION_ADD,              // Color operation: add source and destination colors based on alpha
	SDL_BLENDFACTOR_ONE,                 // Source alpha factor
	SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, // Destination alpha factor: inverse of source alpha
	SDL_BLENDOPERATION_ADD               // Alpha operation: add alpha values
);

struct Point2D {
	double x;
	double y;
	constexpr Point2D(double x, double y) : x(x), y(y) {}
};

static std::array<double, 9> computePerspectiveMatrixFromCorners(
	int width,
	int height,
	const std::array<Point2D, 4>& pts);

#ifdef WIN32
static bool IsIntelGPU() {
	Microsoft::WRL::ComPtr<IDXGIFactory> factory;
	if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.GetAddressOf())))) {
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
	for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC desc;
		adapter->GetDesc(&desc);

		// Check if the vendor ID matches Intel's vendor ID
		if (desc.VendorId == 0x8086) { // 0x8086 is the vendor ID for Intel
			return true;
		}
	}

	return false;
}
#endif

// Utility: SDL_AudioFormat -> GStreamer audio/x-raw format string
// SDL_AudioFormat -> GStreamer audio/x-raw "format" string
// Returns nullptr if unknown.
static const char* sdl_to_gst_fmt(Uint16 fmt) {
	switch (fmt) {
		// 8-bit
		case AUDIO_U8:  return "U8";
		case AUDIO_S8:  return "S8";

			// 16-bit
#if defined(AUDIO_U16LSB)
		case AUDIO_U16LSB: return "U16LE";
#endif
#if defined(AUDIO_U16MSB)
		case AUDIO_U16MSB: return "U16BE";
#endif
#if defined(AUDIO_S16LSB)
		case AUDIO_S16LSB: return "S16LE";
#endif
#if defined(AUDIO_S16MSB)
		case AUDIO_S16MSB: return "S16BE";
#endif

			// 24-bit (packed 3 bytes)  if your SDL build exposes these
#if defined(AUDIO_S24LSB)
		case AUDIO_S24LSB: return "S24LE";
#endif
#if defined(AUDIO_S24MSB)
		case AUDIO_S24MSB: return "S24BE";
#endif

			// 32-bit integer
#if defined(AUDIO_S32LSB)
		case AUDIO_S32LSB: return "S32LE";
#endif
#if defined(AUDIO_S32MSB)
		case AUDIO_S32MSB: return "S32BE";
#endif

			// 32-bit float
#if defined(AUDIO_F32LSB)
		case AUDIO_F32LSB: return "F32LE";
#endif
#if defined(AUDIO_F32MSB)
		case AUDIO_F32MSB: return "F32BE";
#endif
	}
	return nullptr;
}

GStreamerVideo::CallbackCtx* GStreamerVideo::cbCtxRef(CallbackCtx* c) {
	if (!c) return nullptr;
	g_ref_count_inc(&c->ref);
	return c;
}

void GStreamerVideo::cbCtxUnref(gpointer data) {
	auto* c = static_cast<CallbackCtx*>(data);
	if (g_ref_count_dec(&c->ref)) {
		delete c;
	}
}

GStreamerVideo::GStreamerVideo(int monitor)

	: monitor_(monitor)

{
	initialize();
	initializePlugins();
}


GStreamerVideo::~GStreamerVideo() {
	if (asyncState_) {
		// Block until any currently running ThreadPool tasks finish
		std::lock_guard<std::mutex> lock(asyncState_->mutex);
		asyncState_->alive.store(false, std::memory_order_release);
	}
	// Detach callbacks from this instance immediately (no UAF even if callbacks fire late)
	if (cbCtx_) {
		cbCtx_->self.store(nullptr, std::memory_order_release); // <--- ATOMIC STORE
	}
	try {
		stop();
	}
	catch (...) {
		// Destructor must not throw. Optionally log the error.
		LOG_ERROR("GStreamerVideo", "Exception in destructor during stop()");
	}
}

gboolean GStreamerVideo::busCallback(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx || !ctx->state || !ctx->state->alive.load(std::memory_order_acquire))
		return TRUE;

	auto* video = ctx->self.load(std::memory_order_acquire);
	if (!video) return TRUE;

	if (!video->pipeline_) return TRUE;

	if (GST_MESSAGE_SRC(msg) != GST_OBJECT(video->pipeline_)) {
		return TRUE;
	}

	switch (GST_MESSAGE_TYPE(msg)) {
		case GST_MESSAGE_ASYNC_START: {
			video->pipeLineReady_.store(false, std::memory_order_release);
			break;
		}

		case GST_MESSAGE_STATE_CHANGED: {
			if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->pipeline_)) {
				GstState oldS, newS, pending;
				gst_message_parse_state_changed(msg, &oldS, &newS, &pending);

				if (pending == GST_STATE_VOID_PENDING) {
					switch (newS) {
						case GST_STATE_PLAYING:
						video->actualState_.store(IVideo::VideoState::Playing, std::memory_order_release);
						if (video->targetState_.load(std::memory_order_acquire) != IVideo::VideoState::None) {
						}
						break;
						case GST_STATE_PAUSED:
						video->actualState_.store(IVideo::VideoState::Paused, std::memory_order_release);
						if (video->targetState_.load(std::memory_order_acquire) != IVideo::VideoState::None) {
						}
						break;
						default:
						// READY and NULL states map to None
						video->actualState_.store(IVideo::VideoState::None, std::memory_order_release);
						break;
					}
				}
			}
			break;
		}

		case GST_MESSAGE_ASYNC_DONE: {
			if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->pipeline_) &&
				video->targetState_.load(std::memory_order_acquire) != IVideo::VideoState::None) {
				GstState current, pending;
				if (gst_element_get_state(video->pipeline_, &current, &pending, 0) == GST_STATE_CHANGE_SUCCESS) {
					if (current == GST_STATE_PAUSED || current == GST_STATE_PLAYING) {
						bool expected = false;
						(void)video->pipeLineReady_.compare_exchange_strong(expected, true,
							std::memory_order_release, std::memory_order_relaxed);
					}
				}
			}
			break;
		}

		case GST_MESSAGE_STREAM_COLLECTION: {
			if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->pipeline_)) {
				GstStreamCollection* collection = nullptr;
				gst_message_parse_stream_collection(msg, &collection);
				int nVideo = 0;
				if (collection) {
					const guint size = gst_stream_collection_get_size(collection);
					for (guint i = 0; i < size; ++i) {
						GstStream* stream = gst_stream_collection_get_stream(collection, i);
						if (stream && (gst_stream_get_stream_type(stream) & GST_STREAM_TYPE_VIDEO))
							++nVideo;
					}
					gst_object_unref(collection);
				}
				video->hasVideoStream_.store(nVideo > 0, std::memory_order_release);
			}
			break;
		}

		case GST_MESSAGE_EOS: {
			if (GST_MESSAGE_SRC(msg) == GST_OBJECT(video->pipeline_)) {
				if (video->targetState_.load(std::memory_order_acquire) != IVideo::VideoState::None) {
					if (video->pipeline_ && video->getCurrent() > GST_SECOND / 2) {
						video->playCount_++;
						if (!video->numLoops_ || video->numLoops_ > video->playCount_) {
							video->loop();
						}
						else {
							video->loopsFinished_.store(true, std::memory_order_release);
							video->targetState_.store(IVideo::VideoState::None, std::memory_order_release);
							// We set the target state to None, then trigger the state change.
							// The actualState_ update will happen via the switch case above.
							gst_element_set_state(video->pipeline_, GST_STATE_READY);
							video->playCount_ = 0;
						}
					}
					else if (video->pipeline_) {
						video->loopsFinished_.store(true, std::memory_order_release);
						video->targetState_.store(IVideo::VideoState::None, std::memory_order_release);
						gst_element_set_state(video->pipeline_, GST_STATE_READY);
						video->playCount_ = 0;
					}
				}
			}
			break;
		}

		case GST_MESSAGE_ERROR: {
			GError* err = nullptr; gchar* dbg = nullptr;
			gst_message_parse_error(msg, &err, &dbg);
			const bool unloading = (video->targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None);
			if (!unloading) {
				video->hasError_.store(true, std::memory_order_release);
				video->pipeLineReady_.store(false, std::memory_order_release);
				video->targetState_.store(IVideo::VideoState::None, std::memory_order_release);
				gst_element_set_state(video->pipeline_, GST_STATE_READY);
			}
			if (err) g_error_free(err);
			if (dbg) g_free(dbg);
			break;
		}

		default: break;
	}

	return TRUE;
}

void GStreamerVideo::initializePlugins() {
	if (!pluginsInitialized_)
	{
		pluginsInitialized_ = true;

#if defined(WIN32)
		enablePlugin("directsoundsink");
		disablePlugin("mfdeviceprovider");
		disablePlugin("nvh264dec");
		disablePlugin("nvh265dec");
		if (Configuration::HardwareVideoAccel)
		{
			if (IsIntelGPU())
			{
				enablePlugin("qsvh264dec");
				enablePlugin("qsvh265dec");
				disablePlugin("d3d11h264dec");
				disablePlugin("d3d11h265dec");
				disablePlugin("d3d12h264dec");
				disablePlugin("d3d12h265dec");

				LOG_DEBUG("GStreamerVideo", "Using qsvh264dec/qsvh265dec for Intel GPU");
			}
			else
			{
				enablePlugin("d3d12h264dec");
				enablePlugin("d3d12h265dec");
				disablePlugin("qsvh264dec");
				disablePlugin("qsvh265dec");

				LOG_DEBUG("GStreamerVideo", "Using d3d11h264dec/d3d11h265dec for non-Intel GPU");
			}
		}
		else
		{
			enablePlugin("avdec_h264");
			enablePlugin("avdec_h265");
			disablePlugin("d3d11h264dec");
			disablePlugin("d3d11h265dec");
			disablePlugin("d3d12h264dec");
			disablePlugin("d3d12h265dec");
			disablePlugin("qsvh264dec");
			disablePlugin("qsvh265dec");
			LOG_DEBUG("GStreamerVideo", "Using avdec_h264/avdec_h265 for software decoding");
		}
#elif defined(__APPLE__)
		if (!Configuration::HardwareVideoAccel) {
			enablePlugin("avdec_h264");
			enablePlugin("avdec_h265");
			LOG_DEBUG("GStreamerVideo", "Using avdec_h264/avdec_h265 for software decoding");
		}
#else
		//enablePlugin("pipewiresink");
		//disablePlugin("alsasink");
		//disablePlugin("pulsesink");
		if (Configuration::HardwareVideoAccel)
		{
			enablePlugin("vah264dec");
			enablePlugin("vah265dec");
		}
		if (!Configuration::HardwareVideoAccel)
		{
			disablePlugin("vah264dec");
			disablePlugin("vah265dec");
			//enablePlugin("openh264dec");
			enablePlugin("avdec_h264");
			enablePlugin("avdec_h265");
		}
#endif
	}
}

void GStreamerVideo::setNumLoops(int n) {
	if (n > 0)
		numLoops_.store(n, std::memory_order_release);
}

SDL_Texture* GStreamerVideo::getTexture() const {
	if (!isTextureReady_) return nullptr;
	return texture_;
}

bool GStreamerVideo::initialize() {
	if (initialized_) return true;
	if (!gst_is_initialized())
	{
		LOG_DEBUG("GStreamer", "Initializing in instance");
		gst_init(nullptr, nullptr);
		std::string path = Utils::combinePath(Configuration::absolutePath, "retrofe");
#ifdef WIN32
		//GstRegistry* registry = gst_registry_get();
		//gst_registry_scan_path(registry, path.c_str());
#endif
	}
	initialized_ = true;
	return true;
}

bool GStreamerVideo::deInitialize() {
	gst_deinit();
	initialized_ = false;
	return true;
}

namespace {
	void detachAndDrainSink(GstElement* sink, guint* probeId /*nullable*/) {
		if (!sink || !GST_IS_APP_SINK(sink)) return;

		if (probeId && *probeId != 0) {
			if (GstPad* pad = gst_element_get_static_pad(GST_ELEMENT(sink), "sink")) {
				gst_pad_remove_probe(pad, *probeId);
				gst_object_unref(pad);
			}
			*probeId = 0;
		}

		while (GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0)) gst_sample_unref(s);
		while (GstSample* s = gst_app_sink_try_pull_preroll(GST_APP_SINK(sink), 0)) gst_sample_unref(s);
	}
}

void GStreamerVideo::destroyTextures() {
	if (texture_) {
		SDL_DestroyTexture(texture_);
		texture_ = nullptr;
	}
	isTextureReady_ = false;
	allocatedWidth_ = 0;
	allocatedHeight_ = 0;
	allocatedFormat_ = SDL_PIXELFORMAT_UNKNOWN;
}

bool GStreamerVideo::stop() {
	const std::string currentFileForLog = currentFile_;
	LOG_INFO("GStreamerVideo", "Stop (full cleanup) called for " +
		(currentFileForLog.empty() ? "unspecified video" : currentFileForLog));

	// 0) Detach callbacks immediately to avoid any UAF.
	if (cbCtx_) {
		cbCtx_->self.store(nullptr, std::memory_order_release); // <--- ATOMIC STORE
	}

	// 1) Snapshot what we need under the mutex, then release it.
	GstElement* pipeline = nullptr;
	GstElement* videoSink = nullptr;
	GstElement* audioSink = nullptr;
	guint padProbeId = 0;
	guint busWatchId = 0;
	guint elementSetupHandlerId = 0;
	AudioBus::SourceId videoSourceId = 0;

	{
		std::lock_guard<std::mutex> lock(asyncState_->mutex);

		pipeLineReady_.store(false, std::memory_order_release);
		targetState_.store(IVideo::VideoState::None, std::memory_order_release);

		pipeline = pipeline_;
		videoSink = videoSink_;
		audioSink = audioSink_;
		padProbeId = padProbeId_;
		busWatchId = std::exchange(busWatchId_, 0);
		elementSetupHandlerId = elementSetupHandlerId_;
		videoSourceId = videoSourceId_;

		// We’ll remove probe + drain sinks while we still have pointers,
		// but do it outside the lock (see below).
	}

	// 2) Remove probe + drain sinks (no asyncState_ lock needed here).
	//    (Your helper removes the probe if probeId is non-zero and drains appsink queues.)
	if (videoSink) {
		// Use the member padProbeId_ for consistency; we already snapped padProbeId though.
		// Prefer: detachAndDrainSink(videoSink_, &padProbeId_);
		detachAndDrainSink(videoSink, &padProbeId_);
	}
	if (audioSink) {
		guint noProbe = 0;
		detachAndDrainSink(audioSink, &noProbe);
	}

	// Also make sure the member reflects probe removal.
	padProbeId_ = 0;

	// 3) Audio bus cleanup (safe without asyncState_ lock)
	if (videoSourceId != 0) {
		AudioBus::instance().setGain(videoSourceId, 0.0f);
		AudioBus::instance().removeSource(videoSourceId);
		videoSourceId_ = 0;
	}

	// 4) Stop bus delivery before destroying the watch source.
	if (pipeline) {
		if (GstBus* bus = gst_element_get_bus(pipeline)) {
			gst_bus_set_flushing(bus, TRUE);
			gst_object_unref(bus);
		}
	}

	// 5) Destroy the bus watch source on the GLib thread WITHOUT holding asyncState_->mutex.
	if (busWatchId != 0) {
		GlibLoop::instance().invokeAndWait([busWatchId] {
			if (GMainContext* ctx = g_main_context_get_thread_default()) {
				if (GSource* src = g_main_context_find_source_by_id(ctx, busWatchId)) {
					g_source_destroy(src);
				}
			}
			});
	}

	// 6) Now perform GStreamer teardown.
	if (pipeline) {
		gst_element_set_state(pipeline, GST_STATE_NULL);
		(void)gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND);

		// Disconnect signal handler if still connected
		if (elementSetupHandlerId != 0 && pipeline &&
			g_signal_handler_is_connected(pipeline, elementSetupHandlerId)) {
			g_signal_handler_disconnect(pipeline, elementSetupHandlerId);
			elementSetupHandlerId_ = 0;
		}

		LOG_DEBUG("GStreamerVideo::stop", "Unreffing pipeline to guarantee final cleanup.");
		gst_object_unref(pipeline);
	}
	else {
		LOG_DEBUG("GStreamerVideo", "stop(): No pipeline_ was active to stop and unref.");
	}

	// 7) Clear member pointers after teardown (no lock needed if stop is destructor-only;
	//    if stop can be called concurrently, you’d want the mutex here).
	pipeline_ = nullptr;
	videoSink_ = nullptr;
	audioSink_ = nullptr;
	perspective_ = nullptr;

	// 8) Render/sample cleanup
	destroyTextures();

	if (GstSample* s = stagedSample_.exchange(nullptr, std::memory_order_acq_rel)) {
		gst_sample_unref(s);
	}

	if (perspective_gva_) {
		g_value_array_free(perspective_gva_);
		perspective_gva_ = nullptr;
	}

	dimensions_.store({ -1, -1 }, std::memory_order_release);
	currentFile_.clear();
	playCount_ = 0;
	numLoops_ = 0;
	loopsFinished_.store(false, std::memory_order_release);
	hasVideoStream_.store(true, std::memory_order_release);
	volume_ = 0.0f;

	// 9) Release our owning ref to the callback ctx last.
	if (cbCtx_) {
		cbCtxUnref(cbCtx_);
		cbCtx_ = nullptr;
	}

	LOG_INFO("GStreamerVideo", "stop(): Instance fully stopped and all resources released.");
	return true;
}

bool GStreamerVideo::unload() {
	if (!initialized_ || !pipeline_) return false;

	std::lock_guard<std::mutex> lock(asyncState_->mutex);

	// Invalidate the session so any lingering ThreadPool tasks instantly abort
	currentPlaySessionId_.store(nextUniquePlaySessionId_++, std::memory_order_release);

	// 1. Lock out UI spam
	targetState_.store(IVideo::VideoState::None, std::memory_order_release);

	// 2. STOP THE INTERNAL CLOCK (Fixes the "Ghost State")
	gst_element_set_state(pipeline_, GST_STATE_PAUSED);

	// 3. Visual & Audio disconnect
	isTextureReady_ = false; // <-- The UI now receives nullptr from getTexture()
	// Do NOT destroy or nullify texture_ here! It lives on in VRAM.

	dimensions_.store({ -1, -1 }, std::memory_order_release);

	if (videoSourceId_ != 0) {
		AudioBus::instance().setGain(videoSourceId_, 0.0f);
	}

	// 4. Flush residual buffers so the old frame doesn't flash
	if (GstSample* old = stagedSample_.exchange(nullptr, std::memory_order_acq_rel)) {
		gst_sample_unref(old);
	}

	currentFile_ = "";
	playCount_ = 0;
	numLoops_ = 0;
	hasError_.store(false, std::memory_order_release);
	hasVideoStream_.store(false, std::memory_order_release);
	loopsFinished_.store(false, std::memory_order_release);

	LOG_DEBUG("GStreamerVideo", "Unload: Pipeline paused and returned to pool cleanly.");
	return true;
}

// Function to compute perspective transform from 4 arbitrary points
static inline std::array<double, 9> computePerspectiveMatrixFromCorners(
	int width,
	int height,
	const std::array<Point2D, 4>& pts) {
	constexpr double EPSILON = 1e-9;

	const Point2D A = pts[0];
	const Point2D B = pts[1];
	const Point2D D = pts[2];
	const Point2D C = pts[3];


	double M11 = B.x - C.x;
	double M12 = D.x - C.x;
	double M21 = B.y - C.y;
	double M22 = D.y - C.y;
	double RHS1 = A.x - C.x;
	double RHS2 = A.y - C.y;

	double denom = M11 * M22 - M12 * M21;
	if (std::abs(denom) < EPSILON)
		return { 1,0,0, 0,1,0, 0,0,1 };

	double X = (RHS1 * M22 - RHS2 * M12) / denom; // X = g+1
	double Y = (M11 * RHS2 - M21 * RHS1) / denom; // Y = h+1

	double g = X - 1.0;
	double h = Y - 1.0;

	// Now compute the remaining coefficients using the (1,0) and (0,1) constraints:
	double a = X * B.x - A.x;  // a = (g+1)*B.x - A.x
	double d = X * B.y - A.y;
	double b = Y * D.x - A.x;  // b = (h+1)*D.x - A.x
	double e = Y * D.y - A.y;
	double c = A.x;
	double f = A.y;

	// Construct the forward homography Hf:
	// Hf maps (u,v) in [0,1]² to the quadrilateral.
	std::array<double, 9> Hf = { a, b, c,
								 d, e, f,
								 g, h, 1.0 };

	// --- Invert Hf to obtain H, which maps the quadrilateral to the unit square ---
	double det = Hf[0] * (Hf[4] * Hf[8] - Hf[5] * Hf[7])
		- Hf[1] * (Hf[3] * Hf[8] - Hf[5] * Hf[6])
		+ Hf[2] * (Hf[3] * Hf[7] - Hf[4] * Hf[6]);
	if (std::abs(det) < EPSILON)
		return { 1,0,0, 0,1,0, 0,0,1 };
	double invDet = 1.0 / det;
	std::array<double, 9> H = {
		(Hf[4] * Hf[8] - Hf[5] * Hf[7]) * invDet,
		(Hf[2] * Hf[7] - Hf[1] * Hf[8]) * invDet,
		(Hf[1] * Hf[5] - Hf[2] * Hf[4]) * invDet,
		(Hf[5] * Hf[6] - Hf[3] * Hf[8]) * invDet,
		(Hf[0] * Hf[8] - Hf[2] * Hf[6]) * invDet,
		(Hf[2] * Hf[3] - Hf[0] * Hf[5]) * invDet,
		(Hf[3] * Hf[7] - Hf[4] * Hf[6]) * invDet,
		(Hf[1] * Hf[6] - Hf[0] * Hf[7]) * invDet,
		(Hf[0] * Hf[4] - Hf[1] * Hf[3]) * invDet
	};

	// Normalize so that H[8] == 1.
	for (double& val : H) {
		val /= H[8];
	}
	// --- Scale the first two rows by the output dimensions ---
	H[0] *= width; H[1] *= width; H[2] *= width;
	H[3] *= height; H[4] *= height; H[5] *= height;

	return H;
}

bool GStreamerVideo::createPipelineIfNeeded() {
	if (pipeline_) {
		return true;
	}

	// FIX: Make playbin3 the top-level pipeline. 
	// We assign it to pipeline_ and keep playbin_ as an alias so the rest 
	// of the class doesn't need to be rewritten.
	pipeline_ = gst_element_factory_make("playbin3", "player");

	videoSink_ = gst_element_factory_make("appsink", "video_sink");

	if (!pipeline_ || !videoSink_) {
		LOG_DEBUG("Video", "Could not create GStreamer elements");
		hasError_.store(true, std::memory_order_release);
		return false;
	}

	audioSink_ = gst_element_factory_make("appsink", "audio_sink");
	if (!audioSink_) {
		LOG_ERROR("GStreamerVideo", "Could not create audio appsink");
		hasError_.store(true, std::memory_order_release);
		return false;
	}

	// Create callback ctx (one per pipeline) if missing
	if (!cbCtx_) {
		cbCtx_ = new CallbackCtx;
		g_ref_count_init(&cbCtx_->ref);   // initial refcount = 1 (owned by this instance)
		cbCtx_->state = asyncState_;
		cbCtx_->self.store(this, std::memory_order_release); // <--- ATOMIC STORE
	}

	g_object_set(audioSink_,
		"emit-signals", FALSE,
		"max-buffers", 16,          // Increased from 8 - more buffering
		"qos", FALSE,               // Disable QoS - we want all audio
		"drop", FALSE,              // Don't drop audio samples!
		"sync", TRUE,               // Keep sync on for A/V timing
		"enable-last-sample", FALSE,
		"wait-on-eos", FALSE,
		"async", FALSE,             // Audio doesn't need async state changes
		nullptr);

	// --- ONE-TIME CALLBACK REGISTRATION (AUDIO) ---
	GstAppSinkCallbacks audioCbs = {};
	audioCbs.new_sample = &GStreamerVideo::on_audio_new_sample;

	gst_app_sink_set_callbacks(
		GST_APP_SINK(audioSink_),
		&audioCbs,
		cbCtxRef(cbCtx_),          // user_data
		&GStreamerVideo::cbCtxUnref // destroy_notify
	);

	// Pull the actual device spec SDL_mixer opened
	int rate = AudioBus::instance().dev_rate();
	int channels = AudioBus::instance().dev_channels();
	Uint16 fmt = AudioBus::instance().dev_fmt();

	const char* gstFmt = sdl_to_gst_fmt(fmt);
	if (!gstFmt) {
		// Fallback to S16LE if we see a format we don't map yet
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
		gstFmt = "S16LE";
#else
		gstFmt = "S16BE";
#endif
	}

	// Compose caps string
	std::ostringstream ss;
	ss << "audio/x-raw"
		<< ",format=" << gstFmt
		<< ",layout=interleaved"
		<< ",rate=" << rate
		<< ",channels=" << channels;

	GstCaps* acaps = gst_caps_from_string(ss.str().c_str());
	gst_app_sink_set_caps(GST_APP_SINK(audioSink_), acaps);
	gst_caps_unref(acaps);

	// Force the system clock and disable auto reselection
	GstClock* sys = gst_system_clock_obtain();
	gst_pipeline_use_clock(GST_PIPELINE(pipeline_), sys);
	gst_object_unref(sys);

	// Set playbin flags and properties.
	gint flags = GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO;
	flags &= ~(1 << 4);  // Clear GST_PLAY_FLAG_SOFT_VOLUME
	g_object_set(pipeline_, "flags", flags, "instant-uri", TRUE, nullptr);
	g_object_set(pipeline_, "async-handling", TRUE, nullptr);

	// Configure appsink.
	g_object_set(videoSink_,
		"emit-signals", FALSE,          // fewer wakeups
		"max-buffers", 1,               // tiny queue
		"qos", TRUE,
		"drop", TRUE,           // drop if we fall behind
		"sync", TRUE,
		"enable-last-sample", FALSE,
		NULL);

	// --- ONE-TIME CALLBACK REGISTRATION (VIDEO) ---
	GstAppSinkCallbacks videoCbs = {};
	videoCbs.new_preroll = &GStreamerVideo::on_new_preroll;
	videoCbs.new_sample = &GStreamerVideo::on_new_sample;

	gst_app_sink_set_callbacks(
		GST_APP_SINK(videoSink_),
		&videoCbs,
		cbCtxRef(cbCtx_),
		&GStreamerVideo::cbCtxUnref
	);

	// Set caps depending on whether perspective is enabled.
	GstCaps* videoCaps = nullptr;
	if (hasPerspective_) {
		// Enforce RGBA since the perspective element only accepts RGBA.
		videoCaps = gst_caps_from_string(
			"video/x-raw,format=(string)RGBA,pixel-aspect-ratio=(fraction)1/1");
		sdlFormat_ = SDL_PIXELFORMAT_ABGR8888;
		LOG_DEBUG("GStreamerVideo", "SDL pixel format: SDL_PIXELFORMAT_ABGR8888 (Perspective enabled)");
	}
	else {
		if (Configuration::HardwareVideoAccel) {
			videoCaps = gst_caps_from_string(
				"video/x-raw,format=(string)NV12,pixel-aspect-ratio=(fraction)1/1");
			sdlFormat_ = SDL_PIXELFORMAT_NV12;
			LOG_DEBUG("GStreamerVideo", "SDL pixel format: SDL_PIXELFORMAT_NV12 (HW accel: true)");
		}
		else {
			videoCaps = gst_caps_from_string(
				"video/x-raw,format=(string)I420,pixel-aspect-ratio=(fraction)1/1");
			elementSetupHandlerId_ = g_signal_connect(pipeline_, "element-setup",
				G_CALLBACK(elementSetupCallback), this);
			sdlFormat_ = SDL_PIXELFORMAT_IYUV;
			LOG_DEBUG("GStreamerVideo", "SDL pixel format: SDL_PIXELFORMAT_IYUV (HW accel: false)");
		}
	}
	gst_app_sink_set_caps(GST_APP_SINK(videoSink_), videoCaps);
	gst_caps_unref(videoCaps);

	if (hasPerspective_) {
		// Create and set up the perspective pipeline.
		perspective_ = gst_element_factory_make("perspective", "perspective");
		if (!perspective_) {
			LOG_DEBUG("GStreamerVideo", "Could not create perspective element");
			hasError_.store(true, std::memory_order_release);
			return false;
		}

		GstElement* videoBin = gst_bin_new("video_bin");
		if (!videoBin) {
			LOG_DEBUG("GStreamerVideo", "Could not create video bin");
			hasError_.store(true, std::memory_order_release);
			return false;
		}

		gst_bin_add_many(GST_BIN(videoBin), perspective_, videoSink_, nullptr);

		// Link perspective to appsink.
		if (!gst_element_link(perspective_, videoSink_)) {
			LOG_DEBUG("GStreamerVideo", "Could not link perspective to appsink");
			hasError_.store(true, std::memory_order_release);
			return false;
		}

		// Create a ghost pad to expose the sink pad of the perspective element.
		GstPad* perspectiveSinkPad = gst_element_get_static_pad(perspective_, "sink");
		if (!perspectiveSinkPad) {
			LOG_DEBUG("GStreamerVideo", "Could not get sink pad from perspective element");
			hasError_.store(true, std::memory_order_release);
			return false;
		}
		GstPad* ghostPad = gst_ghost_pad_new("sink", perspectiveSinkPad);
		gst_object_unref(perspectiveSinkPad);
		if (!gst_element_add_pad(videoBin, ghostPad)) {
			LOG_DEBUG("GStreamerVideo", "Could not add ghost pad to video bin");
			hasError_.store(true, std::memory_order_release);
			return false;
		}
		gst_object_ref_sink(videoBin);
		// Set the bin as the video-sink.
		g_object_set(pipeline_, "video-sink", videoBin, nullptr);
		gst_object_unref(videoBin);
	}
	else {
		// Simple pipeline: set appsink directly as video-sink.
		gst_object_ref_sink(videoSink_);
		g_object_set(pipeline_, "video-sink", videoSink_, nullptr);
		gst_object_unref(videoSink_);
	}

	// --- THE UNIVERSAL PAD PROBE ---
	GstPad* sinkPad = gst_element_get_static_pad(videoSink_, "sink");
	if (sinkPad) {
		if (padProbeId_ != 0) {
			gst_pad_remove_probe(sinkPad, padProbeId_);
			padProbeId_ = 0;
		}

		padProbeId_ = gst_pad_add_probe(
			sinkPad,
			GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
			&GStreamerVideo::padProbeCallback,
			cbCtxRef(cbCtx_),                // user_data
			&GStreamerVideo::cbCtxUnref       // destroy_notify
		);

		gst_object_unref(sinkPad);
	}
	else {
		LOG_ERROR("GStreamerVideo", "Failed to get sink pad from videoSink_ for file: " + currentFile_);
		hasError_.store(true);
		return false;
	}
	gst_object_ref_sink(audioSink_);
	g_object_set(pipeline_, "audio-sink", audioSink_, nullptr);
	gst_object_unref(audioSink_);

	if (GstBus* bus = gst_element_get_bus(pipeline_)) {
		busWatchId_ = GlibLoop::instance().addBusWatch(
			bus,
			+[](GstBus* b, GstMessage* m, gpointer ud) -> gboolean {
				return GStreamerVideo::busCallback(b, m, ud);
			},
			cbCtxRef(cbCtx_),                 // user_data
			&GStreamerVideo::cbCtxUnref        // notify
		);
		gst_object_unref(bus);
	}

	return true;
}

bool GStreamerVideo::play(const std::string& file) {
	if (!initialized_) {
		LOG_ERROR("GStreamerVideo", "Play called but GStreamer not initialized for file: " + file);
		hasError_.store(true, std::memory_order_release);
		return false;
	}

	const uint64_t newSessionId = nextUniquePlaySessionId_++;
	currentPlaySessionId_.store(newSessionId, std::memory_order_release);
	loopsFinished_.store(false, std::memory_order_release);
	hasVideoStream_.store(false, std::memory_order_release);

	dimensions_.store({ -1, -1 }, std::memory_order_release);

	currentFile_ = file;

	LOG_DEBUG("GStreamerVideo", "Starting play for " + file + " (Session: " + std::to_string(newSessionId) + ")");

	pipeLineReady_.store(false, std::memory_order_release);
	targetState_.store(IVideo::VideoState::Paused, std::memory_order_release);

	auto state = asyncState_;

	ThreadPool::getInstance().enqueue([this, file, newSessionId, state]() {
		std::lock_guard<std::mutex> lock(state->mutex);
		if (!state->alive.load(std::memory_order_acquire)) return;

		// Fast-scroll abort
		if (currentPlaySessionId_.load(std::memory_order_acquire) != newSessionId) {
			return;
		}

		if (!createPipelineIfNeeded()) {
			LOG_ERROR("GStreamerVideo", "Failed to create pipeline for " + file);
			hasError_.store(true, std::memory_order_release);
			return;
		}

		// --- STRICT INSTANT-URI SERIALIZATION LOGIC ---
		GstState current = GST_STATE_NULL, pending = GST_STATE_NULL;

		// 1. Capture the actual return value!
		GstStateChangeReturn ret = gst_element_get_state(pipeline_, &current, &pending, 0);

		// State 1: Hot and ready for instant-uri
		bool isStableActive = (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL) &&
			(current == GST_STATE_PLAYING || current == GST_STATE_PAUSED);

		// State 2: Freshly created or fully torn down (Normal Cold Boot)
		bool isBrandNewOrIdle = (ret == GST_STATE_CHANGE_SUCCESS) &&
			(current == GST_STATE_NULL || current == GST_STATE_READY);

		if (isStableActive) {
			LOG_DEBUG("GStreamerVideo", "Pipeline stable. Executing safe instant-uri switch...");

			gst_element_set_state(pipeline_, GST_STATE_PAUSED);
			
			// Pass nullptr to keep the Pad Probe alive for the new video!
			detachAndDrainSink(videoSink_, nullptr);
			guint noProbe = 0;
			detachAndDrainSink(audioSink_, &noProbe);

			gchar* uri = gst_filename_to_uri(file.c_str(), nullptr);
			g_object_set(pipeline_, "uri", uri, nullptr);
			g_free(uri);

			LOG_DEBUG("GStreamerVideo", "instant-uri switch complete. Prerolling.");
		}
		else {
			if (isBrandNewOrIdle) {
				LOG_DEBUG("GStreamerVideo", "Pipeline is new or idle. Performing standard cold boot.");
			}
			else if (ret == GST_STATE_CHANGE_FAILURE) {
				LOG_WARNING("GStreamerVideo", "Pipeline is in a FAILURE state! Forcing cold boot to reset hardware.");
			}
			else {
				LOG_WARNING("GStreamerVideo", "Pipeline transition overlap (ASYNC) detected! Forcing cold boot fallback.");
			}

			// 1. Turn off the faucet
			gst_element_set_state(pipeline_, GST_STATE_NULL);

			// 2. THE FIX: Pass nullptr here too. We are reusing the appsink, so keep the probe!
			detachAndDrainSink(videoSink_, nullptr);
			guint noProbe = 0;
			detachAndDrainSink(audioSink_, &noProbe);

			gchar* uri = gst_filename_to_uri(file.c_str(), nullptr);
			g_object_set(pipeline_, "uri", uri, nullptr);
			g_free(uri);

			gst_element_set_state(pipeline_, GST_STATE_PAUSED);
		}

		if (videoSourceId_ == 0) {
			videoSourceId_ = AudioBus::instance().addSource("video-preview");
		}
		AudioBus::instance().setGain(videoSourceId_, 0.0f);
		});

	return true;
}


GstPadProbeReturn GStreamerVideo::padProbeCallback(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user_data) {
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx || !ctx->state || !ctx->state->alive.load(std::memory_order_acquire)) {
		return GST_PAD_PROBE_OK;
	}

	auto* self = ctx->self.load(std::memory_order_acquire);
	if (!self) return GST_PAD_PROBE_OK;

	// Lifetime gate: ignore everything during teardown
	auto state = self->asyncState_; // take a local copy
	if (!state || !state->alive.load(std::memory_order_acquire)) {
		return GST_PAD_PROBE_OK;
	}

	GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
	if (GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) {
		GstCaps* caps = nullptr;
		gst_event_parse_caps(ev, &caps);
		if (!caps) return GST_PAD_PROBE_OK;

		const GstStructure* s = gst_caps_get_structure(caps, 0);
		int w = 0, h = 0;
		if (gst_structure_get_int(s, "width", &w) && gst_structure_get_int(s, "height", &h) && w > 0 && h > 0) {

			// 1. Atomic lock-free store of both dimensions simultaneously!
			self->dimensions_.store({ w, h }, std::memory_order_release);

			if (self->hasPerspective_) {
				if (w != self->lastPerspectiveW_ || h != self->lastPerspectiveH_) {
					self->lastPerspectiveW_ = w; self->lastPerspectiveH_ = h;

					// 2. DO THE MATH ON THE BACKGROUND THREAD (Lock-Free!)
					std::array<Point2D, 4> box = {
						Point2D(double(self->perspectiveCorners_[0]), double(self->perspectiveCorners_[1])),
						Point2D(double(self->perspectiveCorners_[2]), double(self->perspectiveCorners_[3])),
						Point2D(double(self->perspectiveCorners_[4]), double(self->perspectiveCorners_[5])),
						Point2D(double(self->perspectiveCorners_[6]), double(self->perspectiveCorners_[7]))
					};
					const auto mat = computePerspectiveMatrixFromCorners(w, h, box);

					// 3. Package the pre-calculated matrix and shared state into the Task
					struct Task {
						GStreamerVideo* self;
						std::shared_ptr<AsyncState> state;
						std::array<double, 9> matrix;
					};

					// MUST use C++ 'new' to properly construct the shared_ptr inside the struct
					auto* task = new Task{ self, self->asyncState_, mat };

					// 4. Send ONLY the memory-update to the Serial GLib thread
					g_main_context_invoke_full(
						/*context*/ GlibLoop::instance().context(),
						/*priority*/ G_PRIORITY_DEFAULT,
						[](gpointer data) -> gboolean {
							Task* t = static_cast<Task*>(data);

							// 5. Lock the independent mutex and verify survival before touching 'self'
							std::lock_guard<std::mutex> lock(t->state->mutex);
							if (!t->state->alive.load(std::memory_order_acquire)) return G_SOURCE_REMOVE;

							auto* v = t->self;
							if (!v || !v->perspective_) return G_SOURCE_REMOVE;

							// 6. Safely manipulate GValueArray on the serialized thread
							if (v->perspective_gva_) {
								g_value_array_free(v->perspective_gva_);
								v->perspective_gva_ = nullptr;
							}
							v->perspective_gva_ = g_value_array_new(9);

							GValue val = G_VALUE_INIT; g_value_init(&val, G_TYPE_DOUBLE);

							// Use the pre-calculated matrix!
							for (double e : t->matrix) {
								g_value_set_double(&val, e);
								g_value_array_append(v->perspective_gva_, &val);
							}
							g_value_unset(&val);

							g_object_set(G_OBJECT(v->perspective_), "matrix", v->perspective_gva_, nullptr);
							return G_SOURCE_REMOVE;
						},
						/*data*/ task,

						// 7. MUST use C++ 'delete' to properly destruct the shared_ptr
						/*destroy_notify*/ [](gpointer data) { delete static_cast<Task*>(data); }
					);
				}
			}
		}
		return GST_PAD_PROBE_OK;
	}
	return GST_PAD_PROBE_OK;
}

void GStreamerVideo::elementSetupCallback([[maybe_unused]] GstElement* playbin,
	GstElement* element,
	[[maybe_unused]] gpointer data) {
	const gchar* name = gst_element_get_name(element);

	auto has_prop = [](GstElement* e, const char* p) {
		return g_object_class_find_property(G_OBJECT_GET_CLASS(e), p) != nullptr;
		};

	// ---- Tune multiqueue to reduce CPU churn ----
	if (g_str_has_prefix(name, "multiqueue")) {
		if (has_prop(element, "max-size-buffers")) g_object_set(element, "max-size-buffers", 0, NULL);
		if (has_prop(element, "max-size-bytes"))   g_object_set(element, "max-size-bytes", (guint64)0, NULL);
		if (has_prop(element, "max-size-time"))    g_object_set(element, "max-size-time", (guint64)(300 * GST_MSECOND), NULL);
		if (has_prop(element, "low-percent"))      g_object_set(element, "low-percent", 30, NULL);
		if (has_prop(element, "high-percent"))     g_object_set(element, "high-percent", 75, NULL);
		if (has_prop(element, "sync-by-running-time"))
			g_object_set(element, "sync-by-running-time", TRUE, NULL);
	}

	// ---- Tune plain queues (if present) ----
	if (g_str_has_prefix(name, "vqueue")) {
		if (has_prop(element, "max-size-buffers")) g_object_set(element, "max-size-buffers", 2, NULL);
		if (has_prop(element, "silent"))           g_object_set(element, "silent", TRUE, NULL);
	}

	// ---- Video decoder settings ----
	if (!Configuration::HardwareVideoAccel && GST_IS_VIDEO_DECODER(element)) {
		g_object_set(element,
			"thread-type", Configuration::AvdecThreadType,
			"max-threads", Configuration::AvdecMaxThreads,
			"direct-rendering", FALSE,
			"std-compliance", 0,
			nullptr);
	}
	g_free((gpointer)name);
}


GstFlowReturn GStreamerVideo::on_new_preroll(GstAppSink* sink, gpointer user_data) {
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx || !ctx->state || !ctx->state->alive.load(std::memory_order_acquire)) {
		return GST_FLOW_OK;
	}

	auto* self = ctx->self.load(std::memory_order_acquire);
	if (!self) return GST_FLOW_OK;

	// If we're logically unloaded/hidden, don't stage frames.
	if (self->targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None) {
		// Don't even pull; reduces work and avoids staging junk.
		return GST_FLOW_OK;
	}

	GstSample* preroll = gst_app_sink_pull_preroll(sink);
	if (!preroll) return GST_FLOW_OK;

	// Teardown race after pull
	if (!ctx->state->alive.load(std::memory_order_acquire) || ctx->self == nullptr) {
		gst_sample_unref(preroll);
		return GST_FLOW_OK;
	}

	// --- THE FIX: ANTI-GHOST FRAME GATE ---
	// If unload() was called on the Main Thread while we were pulling the sample, 
	// discard this frame immediately so it doesn't bleed into the next play session.
	if (self->targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None) {
		gst_sample_unref(preroll);
		return GST_FLOW_OK;
	}

	VideoDim currentDim = self->dimensions_.load(std::memory_order_acquire);
	if (currentDim.w <= 0 || currentDim.h <= 0) {
		if (GstCaps* caps = gst_sample_get_caps(preroll)) {
			GstVideoInfo info;
			if (gst_video_info_from_caps(&info, caps)) {
				self->dimensions_.store(
					{ GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info) },
					std::memory_order_release
				);
			}
		}
	}

	if (GstSample* old = self->stagedSample_.exchange(preroll, std::memory_order_acq_rel)) {
		gst_sample_unref(old);
	}
	return GST_FLOW_OK;
}

GstFlowReturn GStreamerVideo::on_new_sample(GstAppSink* sink, gpointer user_data) {
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx || !ctx->state || !ctx->state->alive.load(std::memory_order_acquire)) {
		return GST_FLOW_OK;
	}

	auto* self = ctx->self.load(std::memory_order_acquire);
	if (!self) return GST_FLOW_OK;

	// Minimal anti-flash gate for unload / hidden
	if (self->targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None) {
		return GST_FLOW_OK;
	}

	GstSample* s = gst_app_sink_pull_sample(sink);
	if (!s) return GST_FLOW_OK;

	// Teardown race after pull
	if (!ctx->state->alive.load(std::memory_order_acquire) || ctx->self == nullptr) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

	// --- THE FIX: ANTI-GHOST FRAME GATE ---
	if (self->targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

	VideoDim currentDim = self->dimensions_.load(std::memory_order_acquire);
	if (currentDim.w <= 0 || currentDim.h <= 0) {
		if (GstCaps* caps = gst_sample_get_caps(s)) {
			GstVideoInfo info;
			if (gst_video_info_from_caps(&info, caps)) {
				self->dimensions_.store(
					{ GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info) },
					std::memory_order_release
				);
			}
		}
	}

	if (GstSample* old = self->stagedSample_.exchange(s, std::memory_order_acq_rel)) {
		gst_sample_unref(old);
	}
	return GST_FLOW_OK;
}

GstFlowReturn GStreamerVideo::on_audio_new_sample(GstAppSink* sink, gpointer user_data) {
	auto* ctx = static_cast<CallbackCtx*>(user_data);
	if (!ctx || !ctx->state || !ctx->state->alive.load(std::memory_order_acquire)) {
		return GST_FLOW_OK;
	}

	auto* self = ctx->self.load(std::memory_order_acquire);
	if (!self) return GST_FLOW_OK;

	// Don't even pull if logically unloaded
	if (self->targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None) {
		return GST_FLOW_OK;
	}

	GstSample* s = gst_app_sink_pull_sample(sink);
	if (!s) return GST_FLOW_OK;

	// Teardown race after pull
	if (!ctx->state->alive.load(std::memory_order_acquire) || ctx->self == nullptr) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

	// --- THE FIX: ANTI-GHOST AUDIO GATE ---
	if (self->targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

	GstBuffer* b = gst_sample_get_buffer(s);
	if (!b || GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_CORRUPTED)) {
		gst_sample_unref(s);
		return GST_FLOW_OK;
	}

	if (GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_DISCONT)) {
		AudioBus::instance().triggerFadeIn(self->videoSourceId_);
	}

	GstMapInfo mi{};
	if (gst_buffer_map(b, &mi, GST_MAP_READ)) {
		AudioBus::instance().push(self->videoSourceId_, mi.data, (int)mi.size);
		gst_buffer_unmap(b, &mi);
	}

	gst_sample_unref(s);
	return GST_FLOW_OK;
}

void GStreamerVideo::createSdlTexture() {
	if (targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None || !pipeline_) {
		return;
	}

	// 1. Snapshot the atomic dimensions ONCE
	VideoDim current = dimensions_.load(std::memory_order_acquire);
	int w = current.w;
	int h = current.h;

	// 2. Compare against our non-atomic "allocated" cache AND check for texture existence
	bool needsRecreate = (allocatedWidth_ != w ||
		allocatedHeight_ != h ||
		allocatedFormat_ != sdlFormat_ ||
		texture_ == nullptr);

	if (!needsRecreate) return;

	// 3. Handle invalid/reset dimensions
	if (w <= 0 || h <= 0) {
		destroyTextures();
		return;
	}

	destroyTextures();

	LOG_INFO("GStreamerVideo", "Creating SDL video texture: " +
		std::to_string(w) + "x" + std::to_string(h) +
		" fmt=" + std::string(SDL_GetPixelFormatName(sdlFormat_)));

	texture_ = SDL_CreateTexture(
		SDL::getRenderer(monitor_), sdlFormat_, SDL_TEXTUREACCESS_STREAMING, w, h);

	if (!texture_) {
		LOG_ERROR("GStreamerVideo", std::string("SDL_CreateTexture failed: ") + SDL_GetError());
		destroyTextures();
		return;
	}

	SDL_SetTextureBlendMode(texture_, softOverlay_ ? softOverlayBlendMode : SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(texture_, SDL_ScaleModeLinear);

	// 4. Update the "last allocated" state
	allocatedWidth_ = w;
	allocatedHeight_ = h;
	allocatedFormat_ = sdlFormat_;
	isTextureReady_ = false;
}

void GStreamerVideo::volumeUpdate() {
	if (!videoSourceId_) return; // Safety check

	// Determine final gain based on UI volume and global mute
	float finalGain = std::clamp(volume_, 0.0f, 1.0f);

	if (Configuration::MuteVideo || finalGain < 0.01f) {
		finalGain = 0.0f;
	}

	// Direct, lightweight update to the mixing bus
	AudioBus::instance().setGain(videoSourceId_, finalGain);
}

void GStreamerVideo::draw() {
	GstSample* sample = stagedSample_.exchange(nullptr, std::memory_order_acq_rel);
	if (!sample) return;

	GstBuffer* buf = gst_sample_get_buffer(sample);
	const GstCaps* caps = gst_sample_get_caps(sample);
	if (!buf || !caps) { gst_sample_unref(sample); return; }

	GstVideoInfo info;
	if (!gst_video_info_from_caps(&info, caps)) { gst_sample_unref(sample); return; }

	const int frameW = GST_VIDEO_INFO_WIDTH(&info);
	const int frameH = GST_VIDEO_INFO_HEIGHT(&info);
	if (frameW <= 0 || frameH <= 0) { gst_sample_unref(sample); return; }

	// Grab a stable snapshot of the atomic dimensions for this frame
	VideoDim currentDim = dimensions_.load(std::memory_order_acquire);

	// 1. Fallback override (just in case the probe missed it)
	if (currentDim.w <= 0 || currentDim.h <= 0) {
		dimensions_.store({ frameW, frameH }, std::memory_order_release);
		currentDim.w = frameW;
		currentDim.h = frameH;
	}

	createSdlTexture();

	// Ref buffer so we can use it after unreffing sample
	gst_buffer_ref(buf);

	// Immediately release the original sample
	gst_sample_unref(sample);

	// Now map buffer
	GstVideoFrame frame;
	if (!gst_video_frame_map(&frame, &info, buf, GST_MAP_READ)) {
		gst_buffer_unref(buf);
		return;
	}

	bool ok = false;
	if (texture_) {
		switch (sdlFormat_) {
			case SDL_PIXELFORMAT_IYUV:     ok = updateTextureFromFrameIYUV(texture_, &frame); break;
			case SDL_PIXELFORMAT_NV12:     ok = updateTextureFromFrameNV12(texture_, &frame); break;
			case SDL_PIXELFORMAT_ABGR8888: ok = updateTextureFromFrameRGBA(texture_, &frame); break;
			default: break;
		}
	}

	// Clean up the copied buffer
	gst_video_frame_unmap(&frame);
	gst_buffer_unref(buf);

	if (ok) {
		// Safe to expose to getTexture() now!
		isTextureReady_ = true;
	}
}

bool GStreamerVideo::updateTextureFromFrameIYUV(SDL_Texture* texture, GstVideoFrame* frame) const {
	const auto* srcY = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 0));
	const auto* srcU = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 1));
	const auto* srcV = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 2));

	// Stride IS the distance between rows in the source buffer
	const int strideY = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
	const int strideU = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 1);
	const int strideV = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 2);

	if (SDL_UpdateYUVTexture(texture, nullptr, srcY, strideY, srcU, strideU, srcV, strideV) != 0) {
		LOG_ERROR("GStreamerVideo", std::string("SDL_UpdateYUVTexture failed: ") + SDL_GetError());
		return false;
	}

	return true;
}

bool GStreamerVideo::updateTextureFromFrameNV12(SDL_Texture* texture, GstVideoFrame* frame) const {
	const auto* srcY = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 0));
	const auto* srcUV = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 1));

	const int strideY = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
	const int strideUV = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 1);

	// Use SDL_UpdateNVTexture for NV12 format
	if (SDL_UpdateNVTexture(texture, nullptr, srcY, strideY, srcUV, strideUV) != 0) {
		LOG_ERROR("GStreamerVideo", std::string("SDL_UpdateNVTexture failed: ") + SDL_GetError());
		return false;
	}

	return true;
}

bool GStreamerVideo::updateTextureFromFrameRGBA(SDL_Texture* texture, GstVideoFrame* frame) const {

	const void* src_pixels = GST_VIDEO_FRAME_PLANE_DATA(frame, 0);
	const int src_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);

	if (SDL_UpdateTexture(texture, nullptr, src_pixels, src_pitch) != 0) {
		LOG_ERROR("GStreamerVideo", "SDL_UpdateTexture failed: " + std::string(SDL_GetError()));
		return false;
	}

	return true;
}

VideoDim GStreamerVideo::getDimensions() {
    return dimensions_.load(std::memory_order_acquire);
}

bool GStreamerVideo::isPlaying() {
	return pipeLineReady_.load(std::memory_order_acquire) && actualState_.load(std::memory_order_acquire) == IVideo::VideoState::Playing;
}

void GStreamerVideo::setVolume(float volume) {
	volume_ = volume;
}

void GStreamerVideo::skipForward() {
	if (!pipeLineReady_.load(std::memory_order_acquire) || !pipeline_) {
		LOG_DEBUG("GStreamerVideo", "skipForward: Pipeline not ready");
		return;
	}

	std::lock_guard<std::mutex> lock(asyncState_->mutex);

	const std::string fileForLog = currentFile_;

	// Check if we're in a seekable state
	GstState current, pending;
	if (gst_element_get_state(pipeline_, &current, &pending, 0) != GST_STATE_CHANGE_SUCCESS) {
		LOG_WARNING("GStreamerVideo", "skipForward: Could not get state for " + fileForLog);
		return;
	}
	if (current != GST_STATE_PLAYING && current != GST_STATE_PAUSED) {
		LOG_DEBUG("GStreamerVideo", "skipForward: Not in seekable state for " + fileForLog);
		return;
	}

	gint64 currentPos = 0;
	gint64 duration = 0;

	if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &currentPos)) {
		LOG_WARNING("GStreamerVideo", "skipForward: Could not query position for " + fileForLog);
		return;
	}
	if (!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &duration)) {
		LOG_WARNING("GStreamerVideo", "skipForward: Could not query duration for " + fileForLog);
		return;
	}

	gint64 newPos = currentPos + (60 * GST_SECOND);
	if (newPos > duration) {
		newPos = duration - (GST_SECOND / 10);  // 100ms from end
	}

	gboolean result = gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
		(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		GST_SEEK_TYPE_SET, newPos,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);

	if (result) {
		LOG_DEBUG("GStreamerVideo", "skipForward: Seeking +60s to " +
			std::to_string(newPos / GST_SECOND) + "s for " + fileForLog);
	}
	else {
		LOG_ERROR("GStreamerVideo", "skipForward: Seek failed for " + fileForLog);
	}
}

void GStreamerVideo::skipBackward() {
	if (!pipeLineReady_.load(std::memory_order_acquire) || !pipeline_) {
		LOG_DEBUG("GStreamerVideo", "skipBackward: Pipeline not ready");
		return;
	}

	std::lock_guard<std::mutex> lock(asyncState_->mutex);

	const std::string fileForLog = currentFile_;

	GstState current, pending;
	if (gst_element_get_state(pipeline_, &current, &pending, 0) != GST_STATE_CHANGE_SUCCESS) {
		LOG_WARNING("GStreamerVideo", "skipBackward: Could not get state for " + fileForLog);
		return;
	}
	if (current != GST_STATE_PLAYING && current != GST_STATE_PAUSED) {
		LOG_DEBUG("GStreamerVideo", "skipBackward: Not in seekable state for " + fileForLog);
		return;
	}

	gint64 currentPos = 0;
	if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &currentPos)) {
		LOG_WARNING("GStreamerVideo", "skipBackward: Could not query position for " + fileForLog);
		return;
	}

	gint64 newPos = (currentPos > 60 * GST_SECOND) ? (currentPos - 60 * GST_SECOND) : 0;

	gboolean result = gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
		(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		GST_SEEK_TYPE_SET, newPos,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);

	if (result) {
		LOG_DEBUG("GStreamerVideo", "skipBackward: Seeking -60s to " +
			std::to_string(newPos / GST_SECOND) + "s for " + fileForLog);
	}
	else {
		LOG_ERROR("GStreamerVideo", "skipBackward: Seek failed for " + fileForLog);
	}
}

void GStreamerVideo::skipForwardp() {
	if (!pipeLineReady_.load(std::memory_order_acquire) || !pipeline_) {
		LOG_DEBUG("GStreamerVideo", "skipForwardp: Pipeline not ready");
		return;
	}

	std::lock_guard<std::mutex> lock(asyncState_->mutex);

	const std::string fileForLog = currentFile_;

	GstState current, pending;
	if (gst_element_get_state(pipeline_, &current, &pending, 0) != GST_STATE_CHANGE_SUCCESS) {
		LOG_WARNING("GStreamerVideo", "skipForwardp: Could not get state for " + fileForLog);
		return;
	}
	if (current != GST_STATE_PLAYING && current != GST_STATE_PAUSED) {
		LOG_DEBUG("GStreamerVideo", "skipForwardp: Not in seekable state for " + fileForLog);
		return;
	}

	gint64 currentPos = 0;
	gint64 duration = 0;

	if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &currentPos)) {
		LOG_WARNING("GStreamerVideo", "skipForwardp: Could not query position for " + fileForLog);
		return;
	}
	if (!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &duration)) {
		LOG_WARNING("GStreamerVideo", "skipForwardp: Could not query duration for " + fileForLog);
		return;
	}

	gint64 skipAmount = duration / 20;  // 5% of duration
	gint64 newPos = currentPos + skipAmount;
	if (newPos > duration) {
		newPos = duration - (GST_SECOND / 10);
	}

	gboolean result = gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
		(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		GST_SEEK_TYPE_SET, newPos,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);

	if (result) {
		LOG_DEBUG("GStreamerVideo", "skipForwardp: Seeking +5% to " +
			std::to_string(newPos / GST_SECOND) + "s for " + fileForLog);
	}
	else {
		LOG_ERROR("GStreamerVideo", "skipForwardp: Seek failed for " + fileForLog);
	}
}

void GStreamerVideo::skipBackwardp() {
	if (!pipeLineReady_.load(std::memory_order_acquire) || !pipeline_) {
		LOG_DEBUG("GStreamerVideo", "skipBackwardp: Pipeline not ready");
		return;
	}

	std::lock_guard<std::mutex> lock(asyncState_->mutex);

	const std::string fileForLog = currentFile_;

	GstState current, pending;
	if (gst_element_get_state(pipeline_, &current, &pending, 0) != GST_STATE_CHANGE_SUCCESS) {
		LOG_WARNING("GStreamerVideo", "skipBackwardp: Could not get state for " + fileForLog);
		return;
	}
	if (current != GST_STATE_PLAYING && current != GST_STATE_PAUSED) {
		LOG_DEBUG("GStreamerVideo", "skipBackwardp: Not in seekable state for " + fileForLog);
		return;
	}

	gint64 currentPos = 0;
	gint64 duration = 0;

	if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &currentPos)) {
		LOG_WARNING("GStreamerVideo", "skipBackwardp: Could not query position for " + fileForLog);
		return;
	}
	if (!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &duration)) {
		LOG_WARNING("GStreamerVideo", "skipBackwardp: Could not query duration for " + fileForLog);
		return;
	}

	gint64 skipAmount = duration / 20;  // 5% of duration
	gint64 newPos = (currentPos > skipAmount) ? (currentPos - skipAmount) : 0;

	gboolean result = gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
		(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		GST_SEEK_TYPE_SET, newPos,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);

	if (result) {
		LOG_DEBUG("GStreamerVideo", "skipBackwardp: Seeking -5% to " +
			std::to_string(newPos / GST_SECOND) + "s for " + fileForLog);
	}
	else {
		LOG_ERROR("GStreamerVideo", "skipBackwardp: Seek failed for " + fileForLog);
	}
}

void GStreamerVideo::pause() {
	if (!pipeLineReady_.load(std::memory_order_acquire)) {
		LOG_DEBUG("GStreamerVideo", "Pause: Pipeline not ready for " + currentFile_);
		return;
	}

	std::lock_guard<std::mutex> lock(asyncState_->mutex);

	if (actualState_.load(std::memory_order_acquire) == IVideo::VideoState::Paused) {
		LOG_DEBUG("GStreamerVideo", "Pause: Already paused for " + currentFile_);
		return;
	}

	if (targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None) {
		return;
	}

	if (targetState_.load(std::memory_order_acquire) == IVideo::VideoState::Paused) {
		LOG_DEBUG("GStreamerVideo", "Pause: Already requesting pause for " + currentFile_);
		return;
	}

	targetState_.store(IVideo::VideoState::Paused, std::memory_order_release);

	const std::string fileForLog = currentFile_;
	LOG_DEBUG("GStreamerVideo", "Requesting PAUSED for " + fileForLog);

	if (!pipeline_) {
		LOG_WARNING("GStreamerVideo", "Pause: Pipeline is null for " + fileForLog);
		hasError_.store(true, std::memory_order_release);
		return;
	}

	// Call directly on the app/render thread. GStreamer handles the internal locking.
	GstStateChangeReturn scr = gst_element_set_state(pipeline_, GST_STATE_PAUSED);

	if (scr == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("GStreamerVideo", "Failed to set PAUSED for " + fileForLog);
		hasError_.store(true, std::memory_order_release);
		targetState_.store(IVideo::VideoState::None, std::memory_order_release);
	}
	else {
		LOG_DEBUG("GStreamerVideo", "Successfully requested PAUSED for " + fileForLog +
			" (change: " + (scr == GST_STATE_CHANGE_ASYNC ? "async" : "immediate") + ")");
	}
}

void GStreamerVideo::resume() {
	if (!pipeLineReady_.load(std::memory_order_acquire)) {
		LOG_DEBUG("GStreamerVideo", "Resume: Pipeline not ready for " + currentFile_);
		return;
	}

	std::lock_guard<std::mutex> lock(asyncState_->mutex);

	// Only return early if actual state is already playing
	if (actualState_.load(std::memory_order_acquire) == IVideo::VideoState::Playing) {
		LOG_DEBUG("GStreamerVideo", "Resume: Already playing for " + currentFile_);
		return;
	}

	// Already requested play?
	if (targetState_.load(std::memory_order_acquire) == IVideo::VideoState::Playing) {
		LOG_DEBUG("GStreamerVideo", "Resume: Already requesting play for " + currentFile_);
		return;
	}

	if (targetState_.load(std::memory_order_acquire) == IVideo::VideoState::None) {
		return;
	}

	// Set target state immediately
	targetState_.store(IVideo::VideoState::Playing, std::memory_order_release);

	const std::string fileForLog = currentFile_;
	LOG_DEBUG("GStreamerVideo", "Requesting PLAYING for " + fileForLog);

	if (!pipeline_) {
		LOG_WARNING("GStreamerVideo", "Resume: Pipeline is null for " + fileForLog);
		hasError_.store(true, std::memory_order_release);
		return;
	}

	// Call directly on the calling thread. GStreamer handles the internal locking.
	GstStateChangeReturn scr = gst_element_set_state(pipeline_, GST_STATE_PLAYING);

	if (scr == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("GStreamerVideo", "Failed to set PLAYING for " + fileForLog);
		hasError_.store(true, std::memory_order_release);
		// Reset target state on failure
		targetState_.store(IVideo::VideoState::None, std::memory_order_release);
	}
	else {
		LOG_DEBUG("GStreamerVideo", "Successfully requested PLAYING for " + fileForLog +
			" (change: " + (scr == GST_STATE_CHANGE_ASYNC ? "async" : "immediate") + ")");
	}
}

void GStreamerVideo::restart() {
	if (!pipeLineReady_.load(std::memory_order_acquire)) {
		LOG_DEBUG("GStreamerVideo", "Restart: Pipeline not ready for " + currentFile_);
		return;
	}

	std::lock_guard<std::mutex> lock(asyncState_->mutex);

	const std::string fileForLog = currentFile_;
	LOG_DEBUG("GStreamerVideo", "Requesting restart (seek to 0) for " + fileForLog);

	if (!pipeline_) {
		LOG_WARNING("GStreamerVideo", "Restart: Pipeline is null for " + fileForLog);
		return;
	}

	// Perform seek operation directly on the calling thread
	gboolean seekResult = gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
		(GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		GST_SEEK_TYPE_SET, 0,
		GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);

	if (seekResult) {
		LOG_DEBUG("GStreamerVideo", "Restart: Successfully sought to beginning for " + fileForLog);
	}
	else {
		LOG_ERROR("GStreamerVideo", "Restart: Failed to seek to start for " + fileForLog);
	}
}

void GStreamerVideo::loop() {
	if (!pipeLineReady_.load(std::memory_order_acquire) || !pipeline_) return;

	auto state = asyncState_; // <--- Grab the shared state
	const std::string fileForLog = currentFile_;

	ThreadPool::getInstance().enqueue([this, fileForLog, state]() {
		// 1. Lock the independent shared mutex FIRST
		std::lock_guard<std::mutex> lock(state->mutex);

		// 2. NOW check if the class memory is still alive
		if (!state->alive.load(std::memory_order_acquire)) return;

		if (!pipeline_) return;

		LOG_DEBUG("GStreamerVideo", "Requesting loop (seek to 0) for " + fileForLog);
		gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
			GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0,
			GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
		});
}

unsigned long long GStreamerVideo::getCurrent() {
	gint64 ret = 0;
	if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &ret) || !pipeLineReady_.load(std::memory_order_acquire))
		ret = 0;
	return (unsigned long long)ret;
}

unsigned long long GStreamerVideo::getDuration() {
	gint64 ret = 0;
	if (!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &ret) || !pipeLineReady_.load(std::memory_order_acquire))
		ret = 0;
	return (unsigned long long)ret;
}

bool GStreamerVideo::isPaused() {
	return pipeLineReady_.load(std::memory_order_acquire) && actualState_.load(std::memory_order_acquire) == IVideo::VideoState::Paused;
}

std::string GStreamerVideo::generateDotFileName(const std::string& prefix, const std::string& videoFilePath) const {
	std::string videoFileName = Utils::getFileName(videoFilePath);

	auto now = std::chrono::system_clock::now();
	auto now_c = std::chrono::system_clock::to_time_t(now);
	auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;

	std::stringstream ss;
	ss << prefix << "_" << videoFileName << "_" << std::put_time(std::localtime(&now_c), "%Y%m%d_%H%M%S_")
		<< std::setfill('0') << std::setw(6) << microseconds.count();

	return ss.str();
}

void GStreamerVideo::enablePlugin(const std::string& pluginName) {
	GstElementFactory* factory = gst_element_factory_find(pluginName.c_str());
	if (factory)
	{
		// Sets the plugin rank to PRIMARY + 1 to prioritize its use
		gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_PRIMARY + 1);
		gst_object_unref(factory);
	}
}

void GStreamerVideo::disablePlugin(const std::string& pluginName) {
	GstElementFactory* factory = gst_element_factory_find(pluginName.c_str());
	if (factory)
	{
		// Sets the plugin rank to GST_RANK_NONE to disable its use
		gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_NONE);
		gst_object_unref(factory);
	}
}

void GStreamerVideo::setSoftOverlay(bool value) {
	softOverlay_ = value;
}

void GStreamerVideo::setPerspectiveCorners(const int* corners) {
	if (corners) {
		std::copy(corners, corners + 8, perspectiveCorners_);
		hasPerspective_ = true;  // Set flag when corners are initialized
	}
	else {
		std::fill(perspectiveCorners_, perspectiveCorners_ + 8, 0);
		hasPerspective_ = false;  // Reset flag when corners are cleared
	}
}

bool GStreamerVideo::hasFinishedLoops() const {
	return loopsFinished_.load(std::memory_order_acquire);
}
