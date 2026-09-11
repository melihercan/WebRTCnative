/*
 *  Peer connection and negotiation.
 *
 *  Everything the caller registers is invoked on WebRTC's signalling thread.
 *  The contract says handlers must not block it — copy, hand off, return.
 *
 *  As in Interop.cc: nothing throws, and every handle is a heap struct owning
 *  a WebRTC smart pointer.
 */

#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Internal.h"
#include "api/jsep.h"
#include "api/media_types.h"
#include "api/rtp_parameters.h"
#include "api/rtp_transceiver_direction.h"
#include "api/rtc_error.h"
#include "rtc_base/logging.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_sender_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"

namespace webrtc_interop {
namespace {

rtc_peer_connection_state ToInterop(
    webrtc::PeerConnectionInterface::PeerConnectionState state) {
  switch (state) {
    case webrtc::PeerConnectionInterface::PeerConnectionState::kNew:
      return RTC_PEER_CONNECTION_STATE_NEW;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting:
      return RTC_PEER_CONNECTION_STATE_CONNECTING;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
      return RTC_PEER_CONNECTION_STATE_CONNECTED;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
      return RTC_PEER_CONNECTION_STATE_DISCONNECTED;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
      return RTC_PEER_CONNECTION_STATE_FAILED;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
      return RTC_PEER_CONNECTION_STATE_CLOSED;
  }
  return RTC_PEER_CONNECTION_STATE_NEW;
}

rtc_signaling_state ToInterop(
    webrtc::PeerConnectionInterface::SignalingState state) {
  switch (state) {
    case webrtc::PeerConnectionInterface::kStable:
      return RTC_SIGNALING_STATE_STABLE;
    case webrtc::PeerConnectionInterface::kHaveLocalOffer:
      return RTC_SIGNALING_STATE_HAVE_LOCAL_OFFER;
    case webrtc::PeerConnectionInterface::kHaveLocalPrAnswer:
      return RTC_SIGNALING_STATE_HAVE_LOCAL_PRANSWER;
    case webrtc::PeerConnectionInterface::kHaveRemoteOffer:
      return RTC_SIGNALING_STATE_HAVE_REMOTE_OFFER;
    case webrtc::PeerConnectionInterface::kHaveRemotePrAnswer:
      return RTC_SIGNALING_STATE_HAVE_REMOTE_PRANSWER;
    case webrtc::PeerConnectionInterface::kClosed:
      return RTC_SIGNALING_STATE_CLOSED;
  }
  return RTC_SIGNALING_STATE_STABLE;
}

/* One per outstanding create-offer or create-answer. WebRTC refcounts it and
 * drops the last reference once it has fired. */
class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  CreateSdpObserver(rtc_on_sdp_success_fn on_success,
                    rtc_on_failure_fn on_failure,
                    void* user_data)
      : on_success_(on_success),
        on_failure_(on_failure),
        user_data_(user_data) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    /* This callback takes ownership of desc. */
    std::unique_ptr<webrtc::SessionDescriptionInterface> owned(desc);
    std::string sdp;
    if (owned == nullptr || !owned->ToString(&sdp)) {
      if (on_failure_ != nullptr) {
        on_failure_(user_data_, "could not serialise the session description");
      }
      return;
    }
    if (on_success_ != nullptr) {
      on_success_(user_data_, owned->type().c_str(), sdp.c_str());
    }
  }

  void OnFailure(webrtc::RTCError error) override {
    if (on_failure_ != nullptr) {
      on_failure_(user_data_, error.message());
    }
  }

 private:
  const rtc_on_sdp_success_fn on_success_;
  const rtc_on_failure_fn on_failure_;
  void* const user_data_;
};

/* One per outstanding get-stats. Refcounted by WebRTC, which drops the last
 * reference once the report has been delivered. */
class StatsCollector : public webrtc::RTCStatsCollectorCallback {
 public:
  StatsCollector(rtc_on_stats_success_fn on_success,
                 rtc_on_failure_fn on_failure,
                 void* user_data)
      : on_success_(on_success),
        on_failure_(on_failure),
        user_data_(user_data) {}

  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report)
      override {
    if (report == nullptr) {
      if (on_failure_ != nullptr) {
        on_failure_(user_data_, "no statistics report was produced");
      }
      return;
    }
    /* ToJson allocates; the string lives for the duration of the call and
     * the caller copies what it wants, as with the SDP callbacks.
     *
     * An empty report serialises to an empty string rather than to "[]", so
     * substitute one. The caller should not have to special-case a payload
     * that is not JSON at all, and a peer connection that has just been
     * created reports nothing. */
    std::string json = report->ToJson();
    if (json.empty()) {
      json = "[]";
    }
    if (on_success_ != nullptr) {
      on_success_(user_data_, json.c_str());
    }
  }

 private:
  const rtc_on_stats_success_fn on_success_;
  const rtc_on_failure_fn on_failure_;
  void* const user_data_;
};

class SetLocalObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  SetLocalObserver(rtc_on_void_success_fn on_success,
                   rtc_on_failure_fn on_failure,
                   void* user_data)
      : on_success_(on_success),
        on_failure_(on_failure),
        user_data_(user_data) {}

  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    if (error.ok()) {
      if (on_success_ != nullptr) {
        on_success_(user_data_);
      }
    } else if (on_failure_ != nullptr) {
      on_failure_(user_data_, error.message());
    }
  }

 private:
  const rtc_on_void_success_fn on_success_;
  const rtc_on_failure_fn on_failure_;
  void* const user_data_;
};

class SetRemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  SetRemoteObserver(rtc_on_void_success_fn on_success,
                    rtc_on_failure_fn on_failure,
                    void* user_data)
      : on_success_(on_success),
        on_failure_(on_failure),
        user_data_(user_data) {}

  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    if (error.ok()) {
      if (on_success_ != nullptr) {
        on_success_(user_data_);
      }
    } else if (on_failure_ != nullptr) {
      on_failure_(user_data_, error.message());
    }
  }

 private:
  const rtc_on_void_success_fn on_success_;
  const rtc_on_failure_fn on_failure_;
  void* const user_data_;
};

}  // namespace

/* Translates WebRTC's C++ callbacks into the flat function pointers the
 * caller registered. Lives as long as its peer connection. */
class InteropObserver : public webrtc::PeerConnectionObserver {
 public:
  InteropObserver(const rtc_peer_connection_observer& callbacks,
                  void* user_data)
      : callbacks_(callbacks), user_data_(user_data) {}

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState state) override {
    if (callbacks_.on_signaling_state != nullptr) {
      callbacks_.on_signaling_state(user_data_, ToInterop(state));
    }
  }

  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState state) override {
    if (callbacks_.on_connection_state != nullptr) {
      callbacks_.on_connection_state(user_data_, ToInterop(state));
    }
  }

  void OnIceCandidate(const webrtc::IceCandidate* candidate) override {
    if (callbacks_.on_ice_candidate == nullptr || candidate == nullptr) {
      return;
    }
    std::string sdp;
    if (!candidate->ToString(&sdp)) {
      return;
    }
    callbacks_.on_ice_candidate(user_data_, candidate->sdp_mid().c_str(),
                                candidate->sdp_mline_index(), sdp.c_str());
  }

  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>
                   transceiver) override {
    if (callbacks_.on_track == nullptr || transceiver == nullptr) {
      return;
    }
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track =
        transceiver->receiver()->track();
    if (track == nullptr) {
      return;
    }

    /* Rule 1: a handle delivered through a callback belongs to the receiver.
     * It is created owning and never released here. */
    rtc_media_track* handle = new (std::nothrow) rtc_media_track();
    if (handle == nullptr) {
      return;
    }
    const bool is_audio =
        track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind;
    handle->track = std::move(track);

    const std::vector<std::string> ids = transceiver->receiver()->stream_ids();
    const std::string stream_id = ids.empty() ? std::string() : ids.front();

    callbacks_.on_track(user_data_, handle,
                        is_audio ? RTC_MEDIA_KIND_AUDIO : RTC_MEDIA_KIND_VIDEO,
                        stream_id.c_str());
  }

  void OnRenegotiationNeeded() override {
    if (callbacks_.on_renegotiation_needed != nullptr) {
      callbacks_.on_renegotiation_needed(user_data_);
    }
  }

  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>
                         channel) override {
    if (callbacks_.on_data_channel == nullptr || channel == nullptr) {
      return;
    }

    /* Rule 1: a handle delivered through a callback belongs to the receiver.
     * Created owning and never released here. The receiver registers its own
     * observer inside the callback, before the channel can open. */
    rtc_data_channel* handle = new (std::nothrow) rtc_data_channel();
    if (handle == nullptr) {
      return;
    }
    handle->channel = std::move(channel);

    callbacks_.on_data_channel(user_data_, handle);
  }

  /* Required by the interface; the aggregate connection state covers it. */
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState
                            /* state */) override {}

 private:
  const rtc_peer_connection_observer callbacks_;
  void* const user_data_;
};

}  // namespace webrtc_interop

/* Out of line so the unique_ptr<InteropObserver> member sees a complete type. */
rtc_peer_connection::rtc_peer_connection() = default;
rtc_peer_connection::~rtc_peer_connection() = default;

/* -------------------------------------------------------------------------
 *  Peer connection
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL
rtc_peer_connection_create(rtc_factory* factory,
                           const rtc_configuration* config,
                           const rtc_peer_connection_observer* observer,
                           void* user_data,
                           rtc_peer_connection** out_pc) {
  if (factory == nullptr || config == nullptr || observer == nullptr ||
      out_pc == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  if (config->ice_server_count < 0 ||
      (config->ice_server_count > 0 && config->ice_servers == nullptr)) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_pc = nullptr;

  webrtc::PeerConnectionInterface::RTCConfiguration configuration;
  configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  for (int32_t i = 0; i < config->ice_server_count; ++i) {
    /* Chromium builds with -Wunsafe-buffer-usage. A C ABI necessarily takes
     * an array as pointer plus count, and the count is validated above, so
     * the suppression is scoped to this one access rather than the target. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    const rtc_ice_server& in = config->ice_servers[i];
#pragma clang diagnostic pop
    if (in.urls == nullptr) {
      return RTC_ERR_INVALID_ARG;
    }
    webrtc::PeerConnectionInterface::IceServer server;
    /* urls arrives comma separated, matching the W3C shape. */
    std::string list(in.urls);
    size_t start = 0;
    while (start <= list.size()) {
      const size_t comma = list.find(',', start);
      const size_t end = comma == std::string::npos ? list.size() : comma;
      std::string url = list.substr(start, end - start);
      if (!url.empty()) {
        server.urls.push_back(url);
      }
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
    if (in.username != nullptr) {
      server.username = in.username;
    }
    if (in.password != nullptr) {
      server.password = in.password;
    }
    configuration.servers.push_back(std::move(server));
  }

  std::unique_ptr<rtc_peer_connection> handle(new (std::nothrow)
                                                  rtc_peer_connection());
  if (handle == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  handle->observer.reset(new (std::nothrow) webrtc_interop::InteropObserver(
      *observer, user_data));
  if (handle->observer == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  webrtc::PeerConnectionDependencies dependencies(handle->observer.get());
  auto result = factory->ptr->CreatePeerConnectionOrError(
      configuration, std::move(dependencies));
  if (!result.ok()) {
    return RTC_ERR_INTERNAL;
  }

  handle->pc = result.MoveValue();
  *out_pc = handle.release();
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_peer_connection_close(rtc_peer_connection* pc) {
  if (pc == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  /* W3C close() is an observable state transition; the handle stays valid so
   * callbacks already in flight can still land. */
  pc->pc->Close();
  return RTC_OK;
}

RTC_API void RTC_CALL rtc_peer_connection_release(rtc_peer_connection* pc) {
  delete pc;
}

/* -------------------------------------------------------------------------
 *  Negotiation
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL
rtc_peer_connection_create_offer(rtc_peer_connection* pc,
                                 rtc_on_sdp_success_fn on_success,
                                 rtc_on_failure_fn on_failure,
                                 void* user_data) {
  if (pc == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  webrtc::scoped_refptr<webrtc_interop::CreateSdpObserver> observer(
      new webrtc::RefCountedObject<webrtc_interop::CreateSdpObserver>(
          on_success, on_failure, user_data));
  pc->pc->CreateOffer(
      observer.get(),
      webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_peer_connection_create_answer(rtc_peer_connection* pc,
                                  rtc_on_sdp_success_fn on_success,
                                  rtc_on_failure_fn on_failure,
                                  void* user_data) {
  if (pc == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  webrtc::scoped_refptr<webrtc_interop::CreateSdpObserver> observer(
      new webrtc::RefCountedObject<webrtc_interop::CreateSdpObserver>(
          on_success, on_failure, user_data));
  pc->pc->CreateAnswer(
      observer.get(),
      webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  return RTC_OK;
}

/* -------------------------------------------------------------------------
 *  Statistics
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL
rtc_peer_connection_get_stats(rtc_peer_connection* pc,
                              rtc_on_stats_success_fn on_success,
                              rtc_on_failure_fn on_failure,
                              void* user_data) {
  if (pc == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  /* Nothing would ever call back, so say so rather than starting a collection
   * whose result is discarded. */
  if (on_success == nullptr && on_failure == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  webrtc::scoped_refptr<webrtc_interop::StatsCollector> collector(
      new webrtc::RefCountedObject<webrtc_interop::StatsCollector>(
          on_success, on_failure, user_data));
  pc->pc->GetStats(collector.get());
  return RTC_OK;
}

namespace {

/* Shared by set_local_description and set_remote_description. */
std::unique_ptr<webrtc::SessionDescriptionInterface> ParseDescription(
    const char* type,
    const char* sdp) {
  const std::optional<webrtc::SdpType> parsed = webrtc::SdpTypeFromString(type);
  if (!parsed.has_value()) {
    return nullptr;
  }
  return webrtc::CreateSessionDescription(*parsed, sdp);
}

}  // namespace

RTC_API rtc_status RTC_CALL
rtc_peer_connection_set_local_description(rtc_peer_connection* pc,
                                          const char* type,
                                          const char* sdp,
                                          rtc_on_void_success_fn on_success,
                                          rtc_on_failure_fn on_failure,
                                          void* user_data) {
  if (pc == nullptr || type == nullptr || sdp == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
      ParseDescription(type, sdp);
  if (desc == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  webrtc::scoped_refptr<webrtc_interop::SetLocalObserver> observer(
      new webrtc::RefCountedObject<webrtc_interop::SetLocalObserver>(
          on_success, on_failure, user_data));
  pc->pc->SetLocalDescription(std::move(desc), observer);
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_peer_connection_set_remote_description(rtc_peer_connection* pc,
                                           const char* type,
                                           const char* sdp,
                                           rtc_on_void_success_fn on_success,
                                           rtc_on_failure_fn on_failure,
                                           void* user_data) {
  if (pc == nullptr || type == nullptr || sdp == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
      ParseDescription(type, sdp);
  if (desc == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  webrtc::scoped_refptr<webrtc_interop::SetRemoteObserver> observer(
      new webrtc::RefCountedObject<webrtc_interop::SetRemoteObserver>(
          on_success, on_failure, user_data));
  pc->pc->SetRemoteDescription(std::move(desc), observer);
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_peer_connection_add_ice_candidate(rtc_peer_connection* pc,
                                      const char* mid,
                                      int32_t mline_index,
                                      const char* sdp) {
  if (pc == nullptr || mid == nullptr || sdp == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  webrtc::SdpParseError error;
  /* This overload returns a raw owning pointer, unlike the one taking a
   * parsed Candidate. Adopt it immediately. */
  std::unique_ptr<webrtc::IceCandidate> candidate(
      webrtc::CreateIceCandidate(mid, mline_index, sdp, &error));
  if (candidate == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  /* Fire and forget: failures surface as a connection state change rather
   * than here, matching the W3C shape. */
  pc->pc->AddIceCandidate(std::move(candidate), [](webrtc::RTCError e) {
    if (!e.ok()) {
      RTC_LOG(LS_ERROR) << "AddIceCandidate rejected: " << e.message();
    }
  });
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_peer_connection_add_track(rtc_peer_connection* pc,
                              rtc_media_track* track,
                              const char* stream_id,
                              rtc_rtp_sender** out_sender) {
  if (pc == nullptr || track == nullptr || stream_id == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  if (out_sender != nullptr) {
    *out_sender = nullptr;
  }

  std::vector<std::string> stream_ids;
  stream_ids.push_back(stream_id);
  auto result = pc->pc->AddTrack(track->track, stream_ids);
  if (!result.ok()) {
    return RTC_ERR_INVALID_STATE;
  }

  /* The track is added either way; the handle is only built when asked for,
   * so a caller that never replaces or removes has nothing to release. */
  if (out_sender == nullptr) {
    return RTC_OK;
  }

  rtc_rtp_sender* handle = new (std::nothrow) rtc_rtp_sender();
  if (handle == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  handle->sender = result.MoveValue();

  *out_sender = handle;
  return RTC_OK;
}

/* -------------------------------------------------------------------------
 *  Senders
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL
rtc_rtp_sender_replace_track(rtc_rtp_sender* sender, rtc_media_track* track) {
  if (sender == nullptr || sender->sender == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }

  /* A null track is meaningful: it stops the sender without renegotiating. */
  webrtc::MediaStreamTrackInterface* raw =
      track == nullptr ? nullptr : track->track.get();

  /* SetTrack fails when the kinds differ or the peer connection is closed; it
   * does not distinguish the two, so report the argument error, which is the
   * one a caller can act on. */
  return sender->sender->SetTrack(raw) ? RTC_OK : RTC_ERR_INVALID_ARG;
}

RTC_API rtc_status RTC_CALL
rtc_peer_connection_remove_track(rtc_peer_connection* pc,
                                 rtc_rtp_sender* sender) {
  if (pc == nullptr || sender == nullptr || sender->sender == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }

  webrtc::RTCError error = pc->pc->RemoveTrackOrError(sender->sender);
  if (!error.ok()) {
    RTC_LOG(LS_ERROR) << "remove_track failed: " << error.message();
    return RTC_ERR_INVALID_STATE;
  }
  return RTC_OK;
}

RTC_API void RTC_CALL rtc_rtp_sender_release(rtc_rtp_sender* sender) {
  delete sender;
}

/* -------------------------------------------------------------------------
 *  Transceivers
 * ---------------------------------------------------------------------- */

namespace webrtc_interop {
namespace {

rtc_rtp_transceiver_direction ToInterop(webrtc::RtpTransceiverDirection d) {
  switch (d) {
    case webrtc::RtpTransceiverDirection::kSendRecv:
      return RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;
    case webrtc::RtpTransceiverDirection::kSendOnly:
      return RTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY;
    case webrtc::RtpTransceiverDirection::kRecvOnly:
      return RTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY;
    case webrtc::RtpTransceiverDirection::kInactive:
      return RTC_RTP_TRANSCEIVER_DIRECTION_INACTIVE;
    case webrtc::RtpTransceiverDirection::kStopped:
      return RTC_RTP_TRANSCEIVER_DIRECTION_STOPPED;
  }
  return RTC_RTP_TRANSCEIVER_DIRECTION_INACTIVE;
}

/* False for anything that is not one of the four settable directions, kStopped
 * included: a transceiver is stopped by stopping it, not by describing it as
 * stopped, and WebRTC rejects that value anyway. */
bool FromInterop(rtc_rtp_transceiver_direction d,
                 webrtc::RtpTransceiverDirection* out) {
  switch (d) {
    case RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV:
      *out = webrtc::RtpTransceiverDirection::kSendRecv;
      return true;
    case RTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY:
      *out = webrtc::RtpTransceiverDirection::kSendOnly;
      return true;
    case RTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY:
      *out = webrtc::RtpTransceiverDirection::kRecvOnly;
      return true;
    case RTC_RTP_TRANSCEIVER_DIRECTION_INACTIVE:
      *out = webrtc::RtpTransceiverDirection::kInactive;
      return true;
    default:
      return false;
  }
}

/* Wraps a transceiver WebRTC already owns. Null only on allocation failure. */
rtc_rtp_transceiver* WrapTransceiver(
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  rtc_rtp_transceiver* handle = new (std::nothrow) rtc_rtp_transceiver();
  if (handle != nullptr) {
    handle->transceiver = std::move(transceiver);
  }
  return handle;
}

/* The optional members carry a sentinel rather than a pointer, so "unset" is
 * decided here rather than by the caller. -1 is impossible for a bitrate or a
 * frame rate, and 0 is impossible for a scale factor that is used as a
 * divisor, which is why those are the sentinels. */
webrtc::RtpEncodingParameters FromInterop(const rtc_rtp_encoding& encoding) {
  webrtc::RtpEncodingParameters out;
  if (encoding.rid != nullptr) {
    out.rid = encoding.rid;
  }
  out.active = encoding.active != 0;
  if (encoding.max_bitrate >= 0) {
    out.max_bitrate_bps = encoding.max_bitrate;
  }
  if (encoding.max_framerate >= 0) {
    out.max_framerate = static_cast<double>(encoding.max_framerate);
  }
  if (encoding.scale_resolution_down_by > 0.0) {
    out.scale_resolution_down_by = encoding.scale_resolution_down_by;
  }
  if (encoding.scalability_mode != nullptr) {
    out.scalability_mode = encoding.scalability_mode;
  }
  return out;
}

}  // namespace
}  // namespace webrtc_interop

RTC_API rtc_status RTC_CALL
rtc_peer_connection_add_transceiver(rtc_peer_connection* pc,
                                    rtc_media_kind kind,
                                    rtc_media_track* track,
                                    rtc_rtp_transceiver_direction direction,
                                    const char* const* stream_ids,
                                    int32_t stream_id_count,
                                    const rtc_rtp_encoding* encodings,
                                    int32_t encoding_count,
                                    rtc_rtp_transceiver** out_transceiver) {
  if (pc == nullptr || out_transceiver == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  if (stream_id_count < 0 || encoding_count < 0) {
    return RTC_ERR_INVALID_ARG;
  }
  if (stream_id_count > 0 && stream_ids == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  if (encoding_count > 0 && encodings == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_transceiver = nullptr;

  webrtc::RtpTransceiverInit init;
  if (!webrtc_interop::FromInterop(direction, &init.direction)) {
    return RTC_ERR_INVALID_ARG;
  }
  for (int32_t i = 0; i < stream_id_count; ++i) {
    if (stream_ids[i] == nullptr) {
      return RTC_ERR_INVALID_ARG;
    }
    init.stream_ids.push_back(stream_ids[i]);
  }
  for (int32_t i = 0; i < encoding_count; ++i) {
    init.send_encodings.push_back(webrtc_interop::FromInterop(encodings[i]));
  }

  webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
      result = [&] {
        if (track != nullptr) {
          /* The track's kind decides the media type; the kind argument is
           * ignored, as the header says. */
          return pc->pc->AddTransceiver(track->track, init);
        }
        const webrtc::MediaType media_type = kind == RTC_MEDIA_KIND_AUDIO
                                                 ? webrtc::MediaType::AUDIO
                                                 : webrtc::MediaType::VIDEO;
        return pc->pc->AddTransceiver(media_type, init);
      }();

  if (!result.ok()) {
    RTC_LOG(LS_ERROR) << "add_transceiver failed: " << result.error().message();
    /* INVALID_PARAMETER means the arguments were wrong -- a null track, or a
     * media type that is neither audio nor video. Anything else is the peer
     * connection refusing in its current state, most often because it is
     * closed. Reporting the two alike would send a caller looking at its own
     * arguments when the connection is simply gone. */
    return result.error().type() == webrtc::RTCErrorType::INVALID_PARAMETER
               ? RTC_ERR_INVALID_ARG
               : RTC_ERR_INVALID_STATE;
  }

  rtc_rtp_transceiver* handle =
      webrtc_interop::WrapTransceiver(result.MoveValue());
  if (handle == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  *out_transceiver = handle;
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_peer_connection_get_transceivers(rtc_peer_connection* pc,
                                     rtc_rtp_transceiver** buffer,
                                     int32_t capacity,
                                     int32_t* out_count) {
  if (pc == nullptr || out_count == nullptr || capacity < 0) {
    return RTC_ERR_INVALID_ARG;
  }

  std::vector<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
      transceivers = pc->pc->GetTransceivers();
  *out_count = static_cast<int32_t>(transceivers.size());

  /* The counting call. */
  if (buffer == nullptr) {
    return RTC_OK;
  }
  if (capacity < *out_count) {
    return RTC_ERR_INVALID_ARG;
  }

  /* Built into a local first, so a failure part way through frees what it
   * allocated instead of leaving the caller a buffer that is partly handles
   * and partly whatever was there before, with no way to tell where the
   * boundary is. */
  std::vector<rtc_rtp_transceiver*> handles;
  handles.reserve(transceivers.size());
  for (auto& transceiver : transceivers) {
    rtc_rtp_transceiver* handle =
        webrtc_interop::WrapTransceiver(std::move(transceiver));
    if (handle == nullptr) {
      for (rtc_rtp_transceiver* allocated : handles) {
        delete allocated;
      }
      return RTC_ERR_INTERNAL;
    }
    handles.push_back(handle);
  }

  for (size_t i = 0; i < handles.size(); ++i) {
    buffer[i] = handles[i];
  }
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_rtp_transceiver_get_mid(rtc_rtp_transceiver* transceiver, char** out_mid) {
  if (transceiver == nullptr || transceiver->transceiver == nullptr ||
      out_mid == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_mid = nullptr;

  const std::optional<std::string> mid = transceiver->transceiver->mid();
  if (!mid.has_value()) {
    return RTC_ERR_NOT_FOUND;
  }

  char* copy = webrtc_interop::DuplicateString(mid->c_str());
  if (copy == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  *out_mid = copy;
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_rtp_transceiver_get_direction(
    rtc_rtp_transceiver* transceiver,
    rtc_rtp_transceiver_direction* out_direction) {
  if (transceiver == nullptr || transceiver->transceiver == nullptr ||
      out_direction == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_direction = webrtc_interop::ToInterop(transceiver->transceiver->direction());
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_rtp_transceiver_get_current_direction(
    rtc_rtp_transceiver* transceiver,
    rtc_rtp_transceiver_direction* out_direction) {
  if (transceiver == nullptr || transceiver->transceiver == nullptr ||
      out_direction == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  const std::optional<webrtc::RtpTransceiverDirection> current =
      transceiver->transceiver->current_direction();
  if (!current.has_value()) {
    return RTC_ERR_NOT_FOUND;
  }
  *out_direction = webrtc_interop::ToInterop(*current);
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_rtp_transceiver_set_direction(
    rtc_rtp_transceiver* transceiver,
    rtc_rtp_transceiver_direction direction) {
  if (transceiver == nullptr || transceiver->transceiver == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  webrtc::RtpTransceiverDirection value;
  if (!webrtc_interop::FromInterop(direction, &value)) {
    return RTC_ERR_INVALID_ARG;
  }
  webrtc::RTCError error =
      transceiver->transceiver->SetDirectionWithError(value);
  if (!error.ok()) {
    RTC_LOG(LS_ERROR) << "set_direction failed: " << error.message();
    return RTC_ERR_INVALID_STATE;
  }
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_rtp_transceiver_get_sender(rtc_rtp_transceiver* transceiver,
                               rtc_rtp_sender** out_sender) {
  if (transceiver == nullptr || transceiver->transceiver == nullptr ||
      out_sender == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_sender = nullptr;

  rtc_rtp_sender* handle = new (std::nothrow) rtc_rtp_sender();
  if (handle == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  handle->sender = transceiver->transceiver->sender();
  *out_sender = handle;
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_rtp_transceiver_get_receiver(rtc_rtp_transceiver* transceiver,
                                 rtc_rtp_receiver** out_receiver) {
  if (transceiver == nullptr || transceiver->transceiver == nullptr ||
      out_receiver == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_receiver = nullptr;

  rtc_rtp_receiver* handle = new (std::nothrow) rtc_rtp_receiver();
  if (handle == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  handle->receiver = transceiver->transceiver->receiver();
  *out_receiver = handle;
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_rtp_transceiver_stop(rtc_rtp_transceiver* transceiver) {
  if (transceiver == nullptr || transceiver->transceiver == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  webrtc::RTCError error = transceiver->transceiver->StopStandard();
  if (!error.ok()) {
    RTC_LOG(LS_ERROR) << "transceiver stop failed: " << error.message();
    return RTC_ERR_INVALID_STATE;
  }
  return RTC_OK;
}

RTC_API void RTC_CALL
rtc_rtp_transceiver_release(rtc_rtp_transceiver* transceiver) {
  delete transceiver;
}

/* -------------------------------------------------------------------------
 *  Receivers
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL
rtc_rtp_receiver_get_track(rtc_rtp_receiver* receiver,
                           rtc_media_track** out_track) {
  if (receiver == nullptr || receiver->receiver == nullptr ||
      out_track == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_track = nullptr;

  webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track =
      receiver->receiver->track();
  if (track == nullptr) {
    return RTC_ERR_NOT_FOUND;
  }

  rtc_media_track* handle = new (std::nothrow) rtc_media_track();
  if (handle == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  /* A remote track, so audio_source stays null: the source belongs to the
   * receiver, which outlives the track handle. */
  handle->track = std::move(track);
  *out_track = handle;
  return RTC_OK;
}

RTC_API void RTC_CALL rtc_rtp_receiver_release(rtc_rtp_receiver* receiver) {
  delete receiver;
}

/* -------------------------------------------------------------------------
 *  Per-sender and per-receiver statistics
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL
rtc_rtp_sender_get_stats(rtc_peer_connection* pc,
                         rtc_rtp_sender* sender,
                         rtc_on_stats_success_fn on_success,
                         rtc_on_failure_fn on_failure,
                         void* user_data) {
  if (pc == nullptr || sender == nullptr || sender->sender == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  if (on_success == nullptr && on_failure == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  webrtc::scoped_refptr<webrtc_interop::StatsCollector> collector(
      new webrtc::RefCountedObject<webrtc_interop::StatsCollector>(
          on_success, on_failure, user_data));
  pc->pc->GetStats(sender->sender, collector);
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_rtp_receiver_get_stats(rtc_peer_connection* pc,
                           rtc_rtp_receiver* receiver,
                           rtc_on_stats_success_fn on_success,
                           rtc_on_failure_fn on_failure,
                           void* user_data) {
  if (pc == nullptr || receiver == nullptr || receiver->receiver == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  if (on_success == nullptr && on_failure == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  webrtc::scoped_refptr<webrtc_interop::StatsCollector> collector(
      new webrtc::RefCountedObject<webrtc_interop::StatsCollector>(
          on_success, on_failure, user_data));
  pc->pc->GetStats(receiver->receiver, collector);
  return RTC_OK;
}
