/*
 *  WebRtcInterop — implementation of include/Interop.h.
 *
 *  Implemented so far: library lifecycle, the peer connection factory, audio
 *  and video device enumeration, and audio and video track creation.
 *
 *  Audio enumeration was stubbed once and this comment went on saying so long
 *  after it was not. It is real, and it carries one workaround: see
 *  CountAudioDevices for the module that stops answering after a call.
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

#include "Internal.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/create_peerconnection_factory.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/adapted_video_track_source.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_sink_interface.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

namespace webrtc_interop {

std::mutex g_mutex;
std::unique_ptr<Runtime> g_runtime;

char* DuplicateString(const char* value) {
  const size_t length = std::strlen(value);
  char* copy = static_cast<char*>(std::malloc(length + 1));
  if (copy == nullptr) {
    return nullptr;
  }
  std::memcpy(copy, value, length + 1);
  return copy;
}

namespace {

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
  /* out_status separates "no such device" from "the device is there but will
   * not start", which are the same nullptr but very different problems: the
   * second is almost always another application holding the camera. Reporting
   * both as NOT_FOUND sends the caller looking for missing hardware. */
  static webrtc::scoped_refptr<CameraSource> Create(const char* device_id,
                                                    int32_t width,
                                                    int32_t height,
                                                    int32_t fps,
                                                    rtc_status* out_status) {
    *out_status = RTC_OK;

    webrtc::scoped_refptr<webrtc::VideoCaptureModule> module =
        webrtc::VideoCaptureFactory::Create(device_id);
    if (module == nullptr) {
      RTC_LOG(LS_ERROR) << "no capture device matches id " << device_id;
      *out_status = RTC_ERR_NOT_FOUND;
      return nullptr;
    }

    /* Ask the device what it has rather than naming a pixel format and
     * letting it convert. Pinning videoType to kI420 -- as this did -- is
     * accepted by every camera and honoured by few: most publish MJPEG, NV12
     * or YUY2 at the larger sizes, so the module quietly picked a nearby
     * capability and converted every frame, MJPEG meaning a full decode per
     * frame on the same thread that feeds the encoder. kUnknown leaves the
     * format unconstrained, which is what makes GetBestMatchedCapability score
     * on size and rate alone; the module still converts to the I420 the track
     * wants, but now from a format the device actually produces. */
    webrtc::VideoCaptureCapability requested;
    requested.width = width;
    requested.height = height;
    requested.maxFPS = fps;
    requested.videoType = webrtc::VideoType::kUnknown;

    webrtc::VideoCaptureCapability capability = requested;
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
    if (info == nullptr ||
        info->GetBestMatchedCapability(device_id, requested, capability) < 0) {
      /* No capability list to choose from. Fall back to the old request
       * exactly, I420 included, rather than starting with kUnknown -- which
       * some backends refuse. */
      capability = requested;
      capability.videoType = webrtc::VideoType::kI420;
      RTC_LOG(LS_WARNING) << "no capability list for " << device_id
                          << "; requesting " << width << "x" << height << "@"
                          << fps << " as I420";
    }

    webrtc::scoped_refptr<CameraSource> source(
        new webrtc::RefCountedObject<CameraSource>());
    module->RegisterCaptureDataCallback(source.get());
    if (module->StartCapture(capability) != 0) {
      module->DeRegisterCaptureDataCallback();
      RTC_LOG(LS_ERROR) << "capture device " << device_id << " exists but would "
                        << "not start at " << capability.width << "x"
                        << capability.height << "@" << capability.maxFPS
                        << "; it is probably in use by another application";
      *out_status = RTC_ERR_INVALID_STATE;
      return nullptr;
    }

    if (capability.width != width || capability.height != height ||
        capability.maxFPS != fps) {
      RTC_LOG(LS_INFO) << "camera " << device_id << " opened at "
                       << capability.width << "x" << capability.height << "@"
                       << capability.maxFPS << ", not the requested " << width
                       << "x" << height << "@" << fps;
    }

    source->capability_ = capability;
    source->module_ = std::move(module);
    return source;
  }

  /* What the device was actually opened at. */
  const webrtc::VideoCaptureCapability& capability() const {
    return capability_;
  }

  /* VideoSinkInterface. Explicitly qualified because AdaptedVideoTrackSource
   * declares a protected OnFrame with the same signature.
   *
   * AdaptFrame is what applies the sinks' wants. Handing the base class every
   * captured frame untouched -- as this did -- loses all of them, and the
   * measured consequences are worth being precise about, because one of the
   * three is not what it looks like:
   *
   *   - max_framerate is lost outright. The source keeps delivering at capture
   *     rate; asking for 10 fps from a 30 fps camera changed nothing.
   *   - The encoder's requests for fewer pixels under CPU or bandwidth
   *     pressure reach the adapter and stop there, so a struggling machine
   *     gets no relief.
   *   - scale_resolution_down_by still WORKS without this, because
   *     VideoStreamEncoder scales a frame that does not match the configured
   *     resolution. What it costs is that the camera goes on capturing and
   *     delivering full-size frames for the encoder to shrink one at a time.
   *
   * So the picture shrinking is not evidence that this is working;
   * media-source width and framesPerSecond are. See test/SenderParameters.c,
   * which measures both against a capturer with and without this call. */
  void OnFrame(const webrtc::VideoFrame& frame) override {
    const int64_t time_us = frame.timestamp_us();
    int out_width = 0;
    int out_height = 0;
    int crop_width = 0;
    int crop_height = 0;
    int crop_x = 0;
    int crop_y = 0;

    if (!AdaptFrame(frame.width(), frame.height(), time_us, &out_width,
                    &out_height, &crop_width, &crop_height, &crop_x,
                    &crop_y)) {
      /* Either no sink wants a frame or the adapter is thinning the rate.
       * Dropping is the normal outcome and AdaptFrame has already told the
       * broadcaster about it, so there is nothing to do but return. */
      return;
    }

    /* The common case by far: nothing is being asked for, so the frame goes
     * through untouched and no buffer is allocated. */
    if (out_width == frame.width() && out_height == frame.height() &&
        crop_width == frame.width() && crop_height == frame.height()) {
      webrtc::AdaptedVideoTrackSource::OnFrame(frame);
      return;
    }

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer =
        frame.video_frame_buffer();
    if (buffer == nullptr) {
      return;
    }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 = buffer->ToI420();
    if (i420 == nullptr) {
      return;
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> adapted =
        webrtc::I420Buffer::Create(out_width, out_height);
    if (adapted == nullptr) {
      return;
    }
    adapted->CropAndScaleFrom(*i420, crop_x, crop_y, crop_width, crop_height);

    /* Rotation is carried rather than applied: the base class applies it when
     * the sinks want it applied, and the renderer is told either way. */
    webrtc::AdaptedVideoTrackSource::OnFrame(
        webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(adapted)
            .set_rotation(frame.rotation())
            .set_timestamp_us(time_us)
            .set_id(frame.id())
            .build());
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
  webrtc::VideoCaptureCapability capability_;
};

}  // namespace
}  // namespace webrtc_interop

using webrtc_interop::DuplicateString;
using webrtc_interop::g_mutex;
using webrtc_interop::g_runtime;
using webrtc_interop::Runtime;

/* -------------------------------------------------------------------------
 *  Library
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL rtc_initialize(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_runtime != nullptr) {
    return RTC_ERR_INVALID_STATE;
  }

  /* WEBRTC_INTEROP_VERBOSE turns on WebRTC's own logging. Kept because the
   * ICE layer is otherwise silent, and the one bug that cost real time here
   * was only visible in it. */
  if (std::getenv("WEBRTC_INTEROP_VERBOSE") != nullptr) {
    webrtc::LogMessage::LogToDebug(webrtc::LS_INFO);
    webrtc::LogMessage::SetLogToStderr(true);
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

  /* The audio device module is created here rather than left to the factory,
   * so device enumeration can report the devices the engine actually uses.
   * It has thread affinity to the worker thread, so it is built there and
   * every later call hops back. A machine with no usable audio device is not
   * a fatal error: the factory falls back to building its own, and
   * enumeration reports RTC_ERR_INTERNAL. */
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      g_runtime->worker_thread->BlockingCall(
          []() -> webrtc::scoped_refptr<webrtc::AudioDeviceModule> {
            webrtc::Environment env = webrtc::CreateEnvironment();
            webrtc::scoped_refptr<webrtc::AudioDeviceModule> module =
                webrtc::CreateAudioDeviceModule(
                    env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
            if (module == nullptr || module->Init() != 0) {
              return nullptr;
            }
            return module;
          });

  /* Null mixer and audio processing still mean "build the platform defaults". */
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory =
      webrtc::CreatePeerConnectionFactory(
          g_runtime->network_thread.get(), g_runtime->worker_thread.get(),
          g_runtime->signaling_thread.get(),
          adm, webrtc::CreateBuiltinAudioEncoderFactory(),
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
  handle->adm = std::move(adm);
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

/* Counts audio devices of one kind, reviving the module if it has been shut
 * down underneath us.
 *
 * AudioDeviceModuleImpl guards its entry points with CHECKinitialized_, which
 * returns -1 once the module has been terminated. Something in a peer
 * connection's lifetime terminates it: measured on Windows 2026-09-14,
 * enumeration returns eight devices before a call and fails for the rest of the
 * process afterwards. The call that provokes it carries no audio track at all -
 * a data channel is enough - so it is not the audio path being used and then
 * released.
 *
 * Init() returns 0 without doing anything when the module is already up, so the
 * retry costs one virtual call in the ordinary case and restores enumeration in
 * the broken one. This treats the symptom rather than the cause, deliberately:
 * whatever terminates the module is inside libwebrtc's own teardown, and a
 * caller asking which devices exist deserves an answer either way.
 *
 * Must be called on the worker thread - the module has thread affinity. */
static int16_t CountAudioDevices(webrtc::AudioDeviceModule* adm,
                                 rtc_audio_device_kind kind) {
  const auto count = [adm, kind]() -> int16_t {
    return kind == RTC_AUDIO_DEVICE_RECORDING ? adm->RecordingDevices()
                                              : adm->PlayoutDevices();
  };

  const int16_t first = count();
  if (first >= 0) {
    return first;
  }
  if (adm->Init() != 0) {
    return -1;
  }
  return count();
}

RTC_API rtc_status RTC_CALL rtc_audio_device_count(rtc_factory* factory,
                                                   rtc_audio_device_kind kind,
                                                   int32_t* out_count) {
  if (factory == nullptr || out_count == nullptr ||
      (kind != RTC_AUDIO_DEVICE_RECORDING && kind != RTC_AUDIO_DEVICE_PLAYOUT)) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_count = 0;
  if (factory->adm == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_runtime == nullptr) {
    return RTC_ERR_INVALID_STATE;
  }

  /* The module has thread affinity to the worker thread. */
  webrtc::AudioDeviceModule* adm = factory->adm.get();
  const int16_t count = g_runtime->worker_thread->BlockingCall(
      [adm, kind] { return CountAudioDevices(adm, kind); });
  if (count < 0) {
    return RTC_ERR_INTERNAL;
  }
  *out_count = count;
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_audio_device_info(rtc_factory* factory,
                                                  rtc_audio_device_kind kind,
                                                  int32_t index,
                                                  char** out_name,
                                                  char** out_id) {
  if (factory == nullptr || out_name == nullptr || out_id == nullptr ||
      index < 0 ||
      (kind != RTC_AUDIO_DEVICE_RECORDING && kind != RTC_AUDIO_DEVICE_PLAYOUT)) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_name = nullptr;
  *out_id = nullptr;
  if (factory->adm == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_runtime == nullptr) {
    return RTC_ERR_INVALID_STATE;
  }

  char name[webrtc::kAdmMaxDeviceNameSize] = {0};
  char guid[webrtc::kAdmMaxGuidSize] = {0};
  webrtc::AudioDeviceModule* adm = factory->adm.get();
  const int32_t result =
      g_runtime->worker_thread->BlockingCall([adm, kind, index, &name, &guid] {
        /* Counted through the same helper as rtc_audio_device_count, so a
         * module that has been terminated is revived here too. */
        const int16_t count = CountAudioDevices(adm, kind);

        /* A module that cannot be revived is an internal fault, not an index
         * nobody has. These used to share a branch, so a caller iterating the
         * devices it had just been told about was told they did not exist. */
        if (count < 0) {
          return -1;
        }
        if (index >= count) {
          return 1; /* out of range, distinguished below */
        }

        const uint16_t i = static_cast<uint16_t>(index);
        return kind == RTC_AUDIO_DEVICE_RECORDING
                   ? adm->RecordingDeviceName(i, name, guid)
                   : adm->PlayoutDeviceName(i, name, guid);
      });
  if (result == 1) {
    return RTC_ERR_NOT_FOUND;
  }
  if (result != 0) {
    return RTC_ERR_INTERNAL;
  }

  /* Some drivers report an empty guid; fall back to the name so the caller
   * always has something to select the device with. */
  char* name_copy = DuplicateString(name);
  char* id_copy = DuplicateString(guid[0] != 0 ? guid : name);
  if (name_copy == nullptr || id_copy == nullptr) {
    std::free(name_copy);
    std::free(id_copy);
    return RTC_ERR_INTERNAL;
  }

  *out_name = name_copy;
  *out_id = id_copy;
  return RTC_OK;
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

  rtc_status create_status = RTC_OK;
  webrtc::scoped_refptr<webrtc_interop::CameraSource> source =
      webrtc_interop::CameraSource::Create(device_id, width, height, fps,
                                           &create_status);
  if (source == nullptr) {
    return create_status;
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
  /* Recorded from the capability that was actually started, not from the
   * arguments: those are a request, and the two differ more often than not. */
  handle->capture_width = source->capability().width;
  handle->capture_height = source->capability().height;
  handle->capture_frame_rate = source->capability().maxFPS;
  *out_track = handle;
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_video_track_get_settings(rtc_media_track* track,
                             int32_t* out_width,
                             int32_t* out_height,
                             int32_t* out_frame_rate) {
  if (track == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  if (track->capture_width == 0 && track->capture_height == 0 &&
      track->capture_frame_rate == 0) {
    return RTC_ERR_NOT_FOUND;
  }

  if (out_width != nullptr) {
    *out_width = track->capture_width;
  }
  if (out_height != nullptr) {
    *out_height = track->capture_height;
  }
  if (out_frame_rate != nullptr) {
    *out_frame_rate = track->capture_frame_rate;
  }
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

RTC_API rtc_status RTC_CALL rtc_media_track_get_kind(rtc_media_track* track,
                                                     rtc_media_kind* out_kind) {
  if (track == nullptr || out_kind == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  /* WebRTC reports the kind as one of two fixed strings rather than as an
   * enum. Anything else would be a track this library did not create, so
   * report the mismatch rather than guessing a kind for it.
   *
   * Only tracks the caller created itself carry a kind it already knows. A
   * track reached through a receiver does not: it arrived through negotiation,
   * and until now the only way to learn its kind was the on_track callback
   * that announced it. */
  const std::string kind = track->track->kind();
  if (kind == webrtc::MediaStreamTrackInterface::kAudioKind) {
    *out_kind = RTC_MEDIA_KIND_AUDIO;
    return RTC_OK;
  }
  if (kind == webrtc::MediaStreamTrackInterface::kVideoKind) {
    *out_kind = RTC_MEDIA_KIND_VIDEO;
    return RTC_OK;
  }
  return RTC_ERR_UNSUPPORTED;
}

RTC_API void RTC_CALL rtc_media_track_release(rtc_media_track* track) {
  delete track;
}
