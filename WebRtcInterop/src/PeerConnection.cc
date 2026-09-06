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
#include <string>
#include <vector>

#include "Internal.h"
#include "api/jsep.h"
#include "api/rtc_error.h"
#include "rtc_base/logging.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_sender_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"

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

  /* Required by the interface, not part of slice one. */
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>
                     /* channel */) override {}
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
                              const char* stream_id) {
  if (pc == nullptr || track == nullptr || stream_id == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  std::vector<std::string> stream_ids;
  stream_ids.push_back(stream_id);
  auto result = pc->pc->AddTrack(track->track, stream_ids);
  if (!result.ok()) {
    return RTC_ERR_INVALID_STATE;
  }
  return RTC_OK;
}
