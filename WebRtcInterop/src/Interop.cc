/*
 *  WebRtcInterop — implementation of include/Interop.h.
 *
 *  Implemented so far: library lifecycle, the peer connection factory, video
 *  device enumeration, and audio and video track creation. Audio device
 *  enumeration is stubbed; see rtc_audio_device_count for why.
 *
 *  Two invariants hold everywhere in this file:
 *
 *    - Nothing throws. WebRTC is compiled with -fno-exceptions, so this
 *      translation unit is too: no try, no catch, and no construct that can
 *      raise. Allocation that may fail uses new (std::nothrow) and is checked.
 *      An exception unwinding into P/Invoke would terminate the process, and
 *      here one cannot arise in the first place.
 *
 *    - A handle is a heap struct owning a WebRTC smart pointer. Reference
 *      counting never crosses the boundary; _release destroys the struct.
 */

#include "Interop.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/create_peerconnection_factory.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/adapted_video_track_source.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

/* -------------------------------------------------------------------------
 *  Handles
 *
 *  Defining the structs the header forward-declares. The smart pointer lives
 *  here, on the heap, for exactly as long as the caller holds the handle.
 * ---------------------------------------------------------------------- */

struct rtc_factory {
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> ptr;
};

struct rtc_media_track {
  webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
  /* Audio tracks do not own their source, so hold a reference here to keep it
   * alive for as long as the caller holds the track. Video tracks do own
   * theirs, and this stays null. */
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source;
};

namespace {

/* The three threads WebRTC needs. Owned by the library, created by
 * rtc_initialize and destroyed by rtc_terminate. Callbacks reach the caller
 * on the signalling thread — see the ABI page. */
struct Runtime {
  std::unique_ptr<webrtc::Thread> network_thread;
  std::unique_ptr<webrtc::Thread> worker_thread;
  std::unique_ptr<webrtc::Thread> signaling_thread;
};

std::mutex g_mutex;
std::unique_ptr<Runtime> g_runtime;

/* Copies into memory the caller frees with rtc_string_free. Returns null only
 * on allocation failure, which callers report as RTC_ERR_INTERNAL. */
char* DuplicateString(const char* value) {
  const size_t length = std::strlen(value);
  char* copy = static_cast<char*>(std::malloc(length + 1));
  if (copy == nullptr) {
    return nullptr;
  }
  std::memcpy(copy, value, length + 1);
  return copy;
}

/* -------------------------------------------------------------------------
 *  Camera source
 *
 *  WebRTC ships no ready-made desktop capturer source: modules/video_capture
 *  produces frames through a VideoSinkInterface, while a track needs a
 *  VideoTrackSourceInterface. AdaptedVideoTrackSource bridges the two, so
 *  this class is a sink that forwards into it.
 *
 *  Frames arrive on the capture thread. AdaptedVideoTrackSource::OnFrame is
 *  documented as safe from any thread, so no locking is needed here.
 * ---------------------------------------------------------------------- */

class CameraSource : public webrtc::AdaptedVideoTrackSource,
                     public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  static webrtc::scoped_refptr<CameraSource> Create(const char* device_id,
                                                    int32_t width,
                                                    int32_t height,
                                                    int32_t fps) {
    webrtc::scoped_refptr<webrtc::VideoCaptureModule> module =
        webrtc::VideoCaptureFactory::Create(device_id);
    if (module == nullptr) {
      return nullptr;
    }

    webrtc::VideoCaptureCapability capability;
    capability.width = width;
    capability.height = height;
    capability.maxFPS = fps;
    capability.videoType = webrtc::VideoType::kI420;

    webrtc::scoped_refptr<CameraSource> source(
        new webrtc::RefCountedObject<CameraSource>());
    module->RegisterCaptureDataCallback(source.get());
    if (module->StartCapture(capability) != 0) {
      module->DeRegisterCaptureDataCallback();
      return nullptr;
    }

    source->module_ = std::move(module);
    return source;
  }

  /* VideoSinkInterface. Explicitly qualified because AdaptedVideoTrackSource
   * declares a protected OnFrame with the same signature. */
  void OnFrame(const webrtc::VideoFrame& frame) override {
    webrtc::AdaptedVideoTrackSource::OnFrame(frame);
  }

  bool is_screencast() const override { return false; }
  std::optional<bool> needs_denoising() const override { return false; }
  webrtc::MediaSourceInterface::SourceState state() const override {
    return module_ != nullptr ? webrtc::MediaSourceInterface::kLive
                              : webrtc::MediaSourceInterface::kEnded;
  }
  bool remote() const override { return false; }

 protected:
  CameraSource() = default;

  ~CameraSource() override {
    if (module_ != nullptr) {
      module_->StopCapture();
      module_->DeRegisterCaptureDataCallback();
    }
  }

 private:
  webrtc::scoped_refptr<webrtc::VideoCaptureModule> module_;
};

}  // namespace

/* -------------------------------------------------------------------------
 *  Library
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL rtc_initialize(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_runtime != nullptr) {
    return RTC_ERR_INVALID_STATE;
  }

  if (!webrtc::InitializeSSL()) {
    return RTC_ERR_INTERNAL;
  }

  std::unique_ptr<Runtime> runtime(new (std::nothrow) Runtime());
  if (runtime == nullptr) {
    webrtc::CleanupSSL();
    return RTC_ERR_INTERNAL;
  }

  /* The network thread needs a socket server; the other two do not. */
  runtime->network_thread = webrtc::Thread::CreateWithSocketServer();
  runtime->worker_thread = webrtc::Thread::Create();
  runtime->signaling_thread = webrtc::Thread::Create();

  if (runtime->network_thread == nullptr || runtime->worker_thread == nullptr ||
      runtime->signaling_thread == nullptr) {
    webrtc::CleanupSSL();
    return RTC_ERR_INTERNAL;
  }

  runtime->network_thread->SetName("webrtc_network", nullptr);
  runtime->worker_thread->SetName("webrtc_worker", nullptr);
  runtime->signaling_thread->SetName("webrtc_signaling", nullptr);

  if (!runtime->network_thread->Start() || !runtime->worker_thread->Start() ||
      !runtime->signaling_thread->Start()) {
    webrtc::CleanupSSL();
    return RTC_ERR_INTERNAL;
  }

  g_runtime = std::move(runtime);
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_terminate(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_runtime == nullptr) {
    return RTC_ERR_INVALID_STATE;
  }

  /* Stop in reverse order of dependency. Any handle still alive at this point
   * is a caller bug; the threads go away underneath it. */
  g_runtime->signaling_thread->Stop();
  g_runtime->worker_thread->Stop();
  g_runtime->network_thread->Stop();
  g_runtime.reset();

  webrtc::CleanupSSL();
  return RTC_OK;
}

RTC_API void RTC_CALL rtc_string_free(char* s) {
  std::free(s);
}

/* -------------------------------------------------------------------------
 *  Factory
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL rtc_factory_create(rtc_factory** out_factory) {
  if (out_factory == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_factory = nullptr;

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_runtime == nullptr) {
    return RTC_ERR_INVALID_STATE;
  }

  /* Null adm, mixer and audio processing mean "build the platform defaults":
   * on Windows that is the WASAPI audio device and the software APM. */
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory =
      webrtc::CreatePeerConnectionFactory(
          g_runtime->network_thread.get(), g_runtime->worker_thread.get(),
          g_runtime->signaling_thread.get(),
          /*default_adm=*/nullptr, webrtc::CreateBuiltinAudioEncoderFactory(),
          webrtc::CreateBuiltinAudioDecoderFactory(),
          webrtc::CreateBuiltinVideoEncoderFactory(),
          webrtc::CreateBuiltinVideoDecoderFactory(),
          /*audio_mixer=*/nullptr, /*audio_processing=*/nullptr);

  if (factory == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  rtc_factory* handle = new (std::nothrow) rtc_factory();
  if (handle == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  handle->ptr = std::move(factory);
  *out_factory = handle;
  return RTC_OK;
}

RTC_API void RTC_CALL rtc_factory_release(rtc_factory* factory) {
  /* Releasing null is a no-op, so a failed create needs no special case on
   * the caller's side. */
  delete factory;
}

/* -------------------------------------------------------------------------
 *  Devices
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL rtc_video_device_count(rtc_factory* factory,
                                                   int32_t* out_count) {
  if (factory == nullptr || out_count == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_count = 0;

  /* DeviceInfo is a raw owning pointer from a factory function, one of the
   * few places WebRTC still hands one out. */
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (info == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  *out_count = static_cast<int32_t>(info->NumberOfDevices());
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_video_device_info(rtc_factory* factory,
                                                  int32_t index,
                                                  char** out_name,
                                                  char** out_id) {
  if (factory == nullptr || out_name == nullptr || out_id == nullptr ||
      index < 0) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_name = nullptr;
  *out_id = nullptr;

  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (info == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  if (static_cast<uint32_t>(index) >= info->NumberOfDevices()) {
    return RTC_ERR_NOT_FOUND;
  }

  char name[256] = {0};
  char id[256] = {0};
  if (info->GetDeviceName(static_cast<uint32_t>(index), name, sizeof(name), id,
                          sizeof(id)) != 0) {
    return RTC_ERR_INTERNAL;
  }

  char* name_copy = DuplicateString(name);
  char* id_copy = DuplicateString(id);
  if (name_copy == nullptr || id_copy == nullptr) {
    std::free(name_copy);
    std::free(id_copy);
    return RTC_ERR_INTERNAL;
  }

  *out_name = name_copy;
  *out_id = id_copy;
  return RTC_OK;
}

/*
 *  Audio device enumeration is not implemented yet.
 *
 *  Unlike video, there is no free-standing enumerator: listing devices needs a
 *  live AudioDeviceModule, and rtc_factory_create currently passes null so the
 *  peer connection factory builds its own internally, out of reach. Getting
 *  one here means creating it ourselves — on Windows that is
 *  CreateWindowsCoreAudioAudioDeviceModule, which requires an Environment and
 *  a COM MTA thread — then passing the same instance to the factory.
 *
 *  That is a real change to how the factory is constructed, so it is its own
 *  slice rather than something to bolt on here. The exports exist and return a
 *  documented status, which is friendlier to the managed side than a missing
 *  entry point.
 */

RTC_API rtc_status RTC_CALL rtc_audio_device_count(rtc_factory* factory,
                                                   int32_t* out_count) {
  if (factory == nullptr || out_count == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_count = 0;
  return RTC_ERR_UNSUPPORTED;
}

RTC_API rtc_status RTC_CALL rtc_audio_device_info(rtc_factory* factory,
                                                  int32_t index,
                                                  char** out_name,
                                                  char** out_id) {
  if (factory == nullptr || out_name == nullptr || out_id == nullptr ||
      index < 0) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_name = nullptr;
  *out_id = nullptr;
  return RTC_ERR_UNSUPPORTED;
}

/* -------------------------------------------------------------------------
 *  Tracks
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL rtc_audio_track_create(rtc_factory* factory,
                                                   const char* label,
                                                   rtc_media_track** out_track) {
  if (factory == nullptr || label == nullptr || out_track == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_track = nullptr;

  /* Default options: the APM settings here are applied globally by the media
   * engine, so AEC, AGC and NS come from the factory's configuration. */
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> source =
      factory->ptr->CreateAudioSource(webrtc::AudioOptions());
  if (source == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  webrtc::scoped_refptr<webrtc::AudioTrackInterface> track =
      factory->ptr->CreateAudioTrack(label, source.get());
  if (track == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  rtc_media_track* handle = new (std::nothrow) rtc_media_track();
  if (handle == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  handle->track = std::move(track);
  handle->audio_source = std::move(source);
  *out_track = handle;
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_video_track_create(rtc_factory* factory,
                                                   const char* device_id,
                                                   const char* label,
                                                   int32_t width,
                                                   int32_t height,
                                                   int32_t fps,
                                                   rtc_media_track** out_track) {
  if (factory == nullptr || device_id == nullptr || label == nullptr ||
      out_track == nullptr || width <= 0 || height <= 0 || fps <= 0) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_track = nullptr;

  webrtc::scoped_refptr<CameraSource> source =
      CameraSource::Create(device_id, width, height, fps);
  if (source == nullptr) {
    return RTC_ERR_NOT_FOUND;
  }

  /* The track takes a reference to the source, so the camera stays open for
   * exactly as long as the track lives. */
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> track =
      factory->ptr->CreateVideoTrack(source, label);
  if (track == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  rtc_media_track* handle = new (std::nothrow) rtc_media_track();
  if (handle == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  handle->track = std::move(track);
  *out_track = handle;
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_media_track_set_enabled(rtc_media_track* track,
                                                        int32_t enabled) {
  if (track == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  track->track->set_enabled(enabled != 0);
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_media_track_get_id(rtc_media_track* track,
                                                   char** out_id) {
  if (track == nullptr || out_id == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_id = nullptr;

  const std::string id = track->track->id();
  char* copy = DuplicateString(id.c_str());
  if (copy == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  *out_id = copy;
  return RTC_OK;
}

RTC_API void RTC_CALL rtc_media_track_release(rtc_media_track* track) {
  delete track;
}
