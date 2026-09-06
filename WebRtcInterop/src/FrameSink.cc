/*
 *  Video frame delivery.
 *
 *  This is the hottest path in the ABI. Frames arrive on a WebRTC capture or
 *  decode thread at the negotiated rate, and the planes they carry belong to
 *  WebRTC for exactly the duration of the callback. The managed handler must
 *  copy or convert and return; anything slower than the frame interval drops
 *  frames or stalls decoding.
 *
 *  As elsewhere: nothing throws, and handles are heap structs owning WebRTC
 *  smart pointers.
 */

#include <new>

#include "Internal.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_sink_interface.h"

namespace webrtc_interop {

/* Registered with a video track; unregistered before it is destroyed. One per
 * track, matching the add/remove pair in the header. */
class FrameSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  FrameSink(rtc_on_frame_fn on_frame, void* user_data)
      : on_frame_(on_frame), user_data_(user_data) {}

  void OnFrame(const webrtc::VideoFrame& frame) override {
    if (on_frame_ == nullptr) {
      return;
    }
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer =
        frame.video_frame_buffer();
    if (buffer == nullptr) {
      return;
    }

    /* ToI420 is a no-op when the frame already is I420, which is the common
     * case for a camera source, and a conversion otherwise. Either way the
     * result lives until this reference goes out of scope, which is why the
     * planes stay valid for exactly the callback. */
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 = buffer->ToI420();
    if (i420 == nullptr) {
      return;
    }

    rtc_video_frame out;
    out.y = i420->DataY();
    out.u = i420->DataU();
    out.v = i420->DataV();
    out.stride_y = i420->StrideY();
    out.stride_u = i420->StrideU();
    out.stride_v = i420->StrideV();
    out.width = i420->width();
    out.height = i420->height();
    out.timestamp_us = frame.timestamp_us();

    on_frame_(user_data_, &out);
  }

 private:
  const rtc_on_frame_fn on_frame_;
  void* const user_data_;
};

}  // namespace webrtc_interop

/* Out of line so the unique_ptr<FrameSink> member sees a complete type. A
 * track released while a sink is registered unregisters it here rather than
 * leaving WebRTC holding a pointer into freed memory. */
rtc_media_track::rtc_media_track() = default;

rtc_media_track::~rtc_media_track() {
  if (sink != nullptr && track != nullptr &&
      track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
    static_cast<webrtc::VideoTrackInterface*>(track.get())->RemoveSink(
        sink.get());
  }
}

/* -------------------------------------------------------------------------
 *  Video frames
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL rtc_video_track_add_sink(rtc_media_track* track,
                                                     rtc_on_frame_fn on_frame,
                                                     void* user_data) {
  if (track == nullptr || on_frame == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  if (track->track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) {
    return RTC_ERR_INVALID_ARG;
  }
  if (track->sink != nullptr) {
    /* One sink per track. Remove the existing one first rather than silently
     * replacing it and leaking the old registration. */
    return RTC_ERR_INVALID_STATE;
  }

  std::unique_ptr<webrtc_interop::FrameSink> sink(
      new (std::nothrow) webrtc_interop::FrameSink(on_frame, user_data));
  if (sink == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  auto* video =
      static_cast<webrtc::VideoTrackInterface*>(track->track.get());
  video->AddOrUpdateSink(sink.get(), webrtc::VideoSinkWants());
  track->sink = std::move(sink);
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_video_track_remove_sink(rtc_media_track* track) {
  if (track == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  if (track->sink == nullptr) {
    return RTC_ERR_INVALID_STATE;
  }

  /* RemoveSink is synchronous: once it returns, no further OnFrame call can
   * be in flight, so destroying the sink afterwards is safe. */
  auto* video =
      static_cast<webrtc::VideoTrackInterface*>(track->track.get());
  video->RemoveSink(track->sink.get());
  track->sink.reset();
  return RTC_OK;
}
