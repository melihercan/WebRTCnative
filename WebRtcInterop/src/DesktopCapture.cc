/*
 *  Screen and window capture — the desktop equivalent of getDisplayMedia.
 *
 *  WebRTC ships modules/desktop_capture but nothing that turns it into a video
 *  track, so this is the bridge: a DesktopCapturer driven by its own thread,
 *  converted to I420, pushed into an AdaptedVideoTrackSource.
 *
 *  As everywhere else here, nothing throws and no C++ type crosses the ABI.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <string>
#include <thread>

#include "Internal.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/adapted_video_track_source.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv/convert.h"

namespace webrtc_interop {
namespace {

std::unique_ptr<webrtc::DesktopCapturer> CreateCapturer(
    rtc_desktop_source_kind kind) {
  /* The default options give the GDI capturers on Windows. DirectX and the
   * Windows Graphics Capture path are faster but need COM or WinRT started on
   * the capture thread, which is a larger contract than this owes its caller. */
  webrtc::DesktopCaptureOptions options =
      webrtc::DesktopCaptureOptions::CreateDefault();

  return kind == RTC_DESKTOP_SOURCE_WINDOW
             ? webrtc::DesktopCapturer::CreateWindowCapturer(options)
             : webrtc::DesktopCapturer::CreateScreenCapturer(options);
}

}  // namespace

/* Owns a capture thread because a DesktopCapturer must be created, used and
 * destroyed on one thread — which cannot be the caller's, since capture has to
 * keep running after the create call returns. */
class DesktopSource : public webrtc::AdaptedVideoTrackSource,
                      public webrtc::DesktopCapturer::Callback {
 public:
  static webrtc::scoped_refptr<DesktopSource> Create(
      rtc_desktop_source_kind kind,
      int64_t source_id,
      int32_t max_fps) {
    webrtc::scoped_refptr<DesktopSource> source(
        new webrtc::RefCountedObject<DesktopSource>());

    source->kind_ = kind;
    source->source_id_ = source_id;
    source->interval_ms_ = 1000 / (max_fps > 0 ? max_fps : 30);
    source->running_ = true;

    /* Started here rather than lazily so a bad source id fails the create call
     * instead of silently producing no frames. */
    source->started_.store(false);
    source->thread_ = std::thread([source]() { source->Run(); });

    /* Wait briefly for the capturer to come up, so the status is meaningful. */
    for (int i = 0; i < 100 && !source->started_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!source->started_.load()) {
      source->Stop();
      return nullptr;
    }

    return source;
  }

  /* DesktopCapturer::Callback, on the capture thread. */
  void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                       std::unique_ptr<webrtc::DesktopFrame> frame) override {
    if (result != webrtc::DesktopCapturer::Result::SUCCESS || frame == nullptr) {
      return;
    }

    const int width = frame->size().width();
    const int height = frame->size().height();
    if (width <= 0 || height <= 0) {
      return;
    }

    /* Same contract as CameraSource::OnFrame in Interop.cc, which carries the
     * full note on what applying the sinks' wants does and does not change.
     * Asked here, before the conversion, so a frame the adapter drops costs no
     * BGRA-to-I420 pass at all -- on a 4K screen that pass is the expensive
     * part of this loop. */
    const int64_t time_us = webrtc::TimeMicros();
    int out_width = 0;
    int out_height = 0;
    int crop_width = 0;
    int crop_height = 0;
    int crop_x = 0;
    int crop_y = 0;

    if (!AdaptFrame(width, height, time_us, &out_width, &out_height,
                    &crop_width, &crop_height, &crop_x, &crop_y)) {
      return;
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> buffer =
        webrtc::I420Buffer::Create(width, height);
    if (buffer == nullptr) {
      return;
    }

    /* DesktopFrame is BGRA in memory, which is what libyuv calls ARGB. */
    if (libyuv::ARGBToI420(frame->data(), frame->stride(),
                           buffer->MutableDataY(), buffer->StrideY(),
                           buffer->MutableDataU(), buffer->StrideU(),
                           buffer->MutableDataV(), buffer->StrideV(),
                           width, height) != 0) {
      return;
    }

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> adapted = buffer;
    if (out_width != width || out_height != height || crop_width != width ||
        crop_height != height) {
      webrtc::scoped_refptr<webrtc::I420Buffer> scaled =
          webrtc::I420Buffer::Create(out_width, out_height);
      if (scaled == nullptr) {
        return;
      }
      scaled->CropAndScaleFrom(*buffer, crop_x, crop_y, crop_width,
                               crop_height);
      adapted = scaled;
    }

    webrtc::AdaptedVideoTrackSource::OnFrame(
        webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(adapted)
            .set_timestamp_us(time_us)
            .set_rotation(webrtc::kVideoRotation_0)
            .build());
  }

  /* True tells the encoder this is screen content, which shifts the
   * degradation preference towards keeping resolution rather than frame rate.
   * Text stays readable when the link tightens; that is the whole difference
   * between a shared screen that is useful and one that is not. */
  bool is_screencast() const override { return true; }

  std::optional<bool> needs_denoising() const override { return false; }

  webrtc::MediaSourceInterface::SourceState state() const override {
    return running_.load() ? webrtc::MediaSourceInterface::kLive
                           : webrtc::MediaSourceInterface::kEnded;
  }

  bool remote() const override { return false; }

 protected:
  DesktopSource() = default;

  ~DesktopSource() override { Stop(); }

 private:
  void Run() {
    std::unique_ptr<webrtc::DesktopCapturer> capturer = CreateCapturer(kind_);
    if (capturer == nullptr) {
      RTC_LOG(LS_ERROR) << "desktop capturer could not be created";
      return;
    }
    if (!capturer->SelectSource(
            static_cast<webrtc::DesktopCapturer::SourceId>(source_id_))) {
      RTC_LOG(LS_ERROR) << "desktop source " << source_id_ << " not found";
      return;
    }

    capturer->Start(this);
    started_.store(true);

    /* Paced to a deadline rather than sleeping a full interval after each
     * capture: capturing a 4K screen through GDI costs tens of milliseconds,
     * and adding that to the interval halves the frame rate the caller asked
     * for. Sleep only for what is left of the period, and if capture already
     * overran it, go straight round again. */
    auto next = std::chrono::steady_clock::now();
    const auto interval = std::chrono::milliseconds(interval_ms_);

    while (running_.load()) {
      capturer->CaptureFrame();

      next += interval;
      const auto now = std::chrono::steady_clock::now();
      if (now < next) {
        std::this_thread::sleep_for(next - now);
      } else {
        next = now;
      }
    }
    /* Destroyed here, on the thread that created it, as the API requires. */
  }

  void Stop() {
    if (!running_.exchange(false)) {
      return;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  rtc_desktop_source_kind kind_ = RTC_DESKTOP_SOURCE_SCREEN;
  int64_t source_id_ = 0;
  int interval_ms_ = 33;
  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};
  std::thread thread_;
};

}  // namespace webrtc_interop

/* -------------------------------------------------------------------------
 *  Desktop capture
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL
rtc_desktop_source_count(rtc_desktop_source_kind kind, int32_t* out_count) {
  if (out_count == nullptr ||
      (kind != RTC_DESKTOP_SOURCE_SCREEN && kind != RTC_DESKTOP_SOURCE_WINDOW)) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_count = 0;

  std::unique_ptr<webrtc::DesktopCapturer> capturer =
      webrtc_interop::CreateCapturer(kind);
  if (capturer == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  webrtc::DesktopCapturer::SourceList sources;
  if (!capturer->GetSourceList(&sources)) {
    return RTC_ERR_INTERNAL;
  }

  *out_count = static_cast<int32_t>(sources.size());
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_desktop_source_info(rtc_desktop_source_kind kind,
                        int32_t index,
                        char** out_title,
                        int64_t* out_id) {
  if (index < 0 || out_title == nullptr || out_id == nullptr ||
      (kind != RTC_DESKTOP_SOURCE_SCREEN && kind != RTC_DESKTOP_SOURCE_WINDOW)) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_title = nullptr;
  *out_id = 0;

  std::unique_ptr<webrtc::DesktopCapturer> capturer =
      webrtc_interop::CreateCapturer(kind);
  if (capturer == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  webrtc::DesktopCapturer::SourceList sources;
  if (!capturer->GetSourceList(&sources)) {
    return RTC_ERR_INTERNAL;
  }
  if (static_cast<size_t>(index) >= sources.size()) {
    return RTC_ERR_NOT_FOUND;
  }

  const webrtc::DesktopCapturer::Source& source = sources[index];

  /* Screens routinely have no title; give the caller something nameable
   * rather than an empty string it has to special-case. */
  const std::string title =
      source.title.empty() ? ("Screen " + std::to_string(index + 1))
                           : source.title;

  char* copy = webrtc_interop::DuplicateString(title.c_str());
  if (copy == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  *out_title = copy;
  *out_id = static_cast<int64_t>(source.id);
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_desktop_track_create(rtc_factory* factory,
                         rtc_desktop_source_kind kind,
                         int64_t source_id,
                         const char* label,
                         int32_t max_fps,
                         rtc_media_track** out_track) {
  if (factory == nullptr || label == nullptr || out_track == nullptr ||
      max_fps <= 0 ||
      (kind != RTC_DESKTOP_SOURCE_SCREEN && kind != RTC_DESKTOP_SOURCE_WINDOW)) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_track = nullptr;

  webrtc::scoped_refptr<webrtc_interop::DesktopSource> source =
      webrtc_interop::DesktopSource::Create(kind, source_id, max_fps);
  if (source == nullptr) {
    return RTC_ERR_NOT_FOUND;
  }

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
