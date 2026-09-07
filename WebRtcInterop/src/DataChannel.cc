/*
 *  Data channels.
 *
 *  The SCTP side of a peer connection: a reliable or unreliable message
 *  stream alongside the media. As everywhere else here, nothing throws, every
 *  handle is a heap struct owning a WebRTC smart pointer, and callbacks reach
 *  the caller on the signalling thread.
 */

#include <memory>
#include <new>
#include <optional>
#include <string>

#include "Internal.h"
#include "api/data_channel_interface.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/logging.h"

namespace webrtc_interop {
namespace {

rtc_data_channel_state ToInterop(webrtc::DataChannelInterface::DataState state) {
  switch (state) {
    case webrtc::DataChannelInterface::kConnecting:
      return RTC_DATA_CHANNEL_STATE_CONNECTING;
    case webrtc::DataChannelInterface::kOpen:
      return RTC_DATA_CHANNEL_STATE_OPEN;
    case webrtc::DataChannelInterface::kClosing:
      return RTC_DATA_CHANNEL_STATE_CLOSING;
    case webrtc::DataChannelInterface::kClosed:
      return RTC_DATA_CHANNEL_STATE_CLOSED;
  }
  return RTC_DATA_CHANNEL_STATE_CLOSED;
}

}  // namespace

/* Holds the channel it observes so OnStateChange can read the new state:
 * WebRTC reports that the state changed, not what it changed to. The pointer
 * is borrowed -- the handle owns both this observer and the channel, and
 * destroys them in an order that keeps this valid. */
class InteropDataChannelObserver : public webrtc::DataChannelObserver {
 public:
  InteropDataChannelObserver(const rtc_data_channel_observer& callbacks,
                             void* user_data,
                             webrtc::DataChannelInterface* channel)
      : callbacks_(callbacks), user_data_(user_data), channel_(channel) {}

  void OnStateChange() override {
    if (callbacks_.on_state != nullptr && channel_ != nullptr) {
      callbacks_.on_state(user_data_, ToInterop(channel_->state()));
    }
  }

  void OnMessage(const webrtc::DataBuffer& buffer) override {
    if (callbacks_.on_message == nullptr) {
      return;
    }
    /* Borrowed for the duration of the call, like every other string and
     * buffer crossing this boundary. */
    callbacks_.on_message(user_data_, buffer.data.cdata(),
                          static_cast<int32_t>(buffer.data.size()),
                          buffer.binary ? 1 : 0);
  }

  void OnBufferedAmountChange(uint64_t sent_data_size) override {
    if (callbacks_.on_buffered_amount_change != nullptr) {
      callbacks_.on_buffered_amount_change(user_data_, sent_data_size);
    }
  }

 private:
  const rtc_data_channel_observer callbacks_;
  void* const user_data_;
  webrtc::DataChannelInterface* const channel_;
};

}  // namespace webrtc_interop

/* Out of line so the unique_ptr<InteropDataChannelObserver> member sees a
 * complete type. */
rtc_data_channel::rtc_data_channel() = default;

rtc_data_channel::~rtc_data_channel() {
  /* The peer connection also holds a reference, so the channel can outlive
   * this handle. Unregister explicitly rather than relying on destruction
   * order, or it would keep calling into a freed observer. */
  if (observer != nullptr && channel != nullptr) {
    channel->UnregisterObserver();
  }
}

/* -------------------------------------------------------------------------
 *  Data channels
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL
rtc_peer_connection_create_data_channel(rtc_peer_connection* pc,
                                        const char* label,
                                        const rtc_data_channel_init* init,
                                        rtc_data_channel** out_channel) {
  if (pc == nullptr || label == nullptr || out_channel == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_channel = nullptr;

  webrtc::DataChannelInit config;
  if (init != nullptr) {
    config.ordered = init->ordered != 0;
    config.negotiated = init->negotiated != 0;
    if (init->protocol != nullptr) {
      config.protocol = init->protocol;
    }
    /* -1 means the caller left it unset; WebRTC wants the optional empty. */
    if (init->max_packet_life_time >= 0) {
      config.maxRetransmitTime = init->max_packet_life_time;
    }
    if (init->max_retransmits >= 0) {
      config.maxRetransmits = init->max_retransmits;
    }
    if (init->id >= 0) {
      config.id = init->id;
    }
  }

  webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::DataChannelInterface>>
      created = pc->pc->CreateDataChannelOrError(label, &config);
  if (!created.ok()) {
    RTC_LOG(LS_ERROR) << "create_data_channel failed: "
                      << created.error().message();
    return RTC_ERR_INTERNAL;
  }

  rtc_data_channel* handle = new (std::nothrow) rtc_data_channel();
  if (handle == nullptr) {
    return RTC_ERR_INTERNAL;
  }
  handle->channel = created.MoveValue();

  *out_channel = handle;
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_data_channel_set_observer(rtc_data_channel* channel,
                              const rtc_data_channel_observer* observer,
                              void* user_data) {
  if (channel == nullptr || channel->channel == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }

  /* Replacing or clearing: drop the old registration first, so the channel is
   * never pointing at an observer that is being destroyed. */
  if (channel->observer != nullptr) {
    channel->channel->UnregisterObserver();
    channel->observer.reset();
  }

  if (observer == nullptr) {
    return RTC_OK;
  }

  std::unique_ptr<webrtc_interop::InteropDataChannelObserver> created(
      new (std::nothrow) webrtc_interop::InteropDataChannelObserver(
          *observer, user_data, channel->channel.get()));
  if (created == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  channel->channel->RegisterObserver(created.get());
  channel->observer = std::move(created);
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_data_channel_send(rtc_data_channel* channel,
                                                  const uint8_t* data,
                                                  int32_t size,
                                                  int32_t is_binary) {
  if (channel == nullptr || channel->channel == nullptr || size < 0 ||
      (size > 0 && data == nullptr)) {
    return RTC_ERR_INVALID_ARG;
  }
  if (channel->channel->state() != webrtc::DataChannelInterface::kOpen) {
    return RTC_ERR_INVALID_STATE;
  }

  /* Chromium builds with -Wunsafe-buffer-usage. A C ABI necessarily takes a
   * buffer as pointer plus count, and the count is validated above, so the
   * suppression is scoped to this one access. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
  webrtc::CopyOnWriteBuffer payload(data, static_cast<size_t>(size));
#pragma clang diagnostic pop

  /* Asynchronous, like create_offer: the status says the send was accepted,
   * not that it was delivered. WebRTC documents Send's own bool as unreliable
   * for exactly this reason, and the W3C send() reports nothing either. */
  channel->channel->SendAsync(
      webrtc::DataBuffer(payload, is_binary != 0),
      [](webrtc::RTCError error) {
        if (!error.ok()) {
          RTC_LOG(LS_ERROR) << "data channel send failed: " << error.message();
        }
      });

  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_data_channel_get_label(rtc_data_channel* channel, char** out_label) {
  if (channel == nullptr || channel->channel == nullptr ||
      out_label == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_label = nullptr;

  const std::string label = channel->channel->label();
  char* copy = webrtc_interop::DuplicateString(label.c_str());
  if (copy == nullptr) {
    return RTC_ERR_INTERNAL;
  }

  *out_label = copy;
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_data_channel_get_id(rtc_data_channel* channel,
                                                    int32_t* out_id) {
  if (channel == nullptr || channel->channel == nullptr || out_id == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_id = static_cast<int32_t>(channel->channel->id());
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_data_channel_get_state(rtc_data_channel* channel,
                           rtc_data_channel_state* out_state) {
  if (channel == nullptr || channel->channel == nullptr ||
      out_state == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_state = webrtc_interop::ToInterop(channel->channel->state());
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL
rtc_data_channel_get_buffered_amount(rtc_data_channel* channel,
                                     uint64_t* out_amount) {
  if (channel == nullptr || channel->channel == nullptr ||
      out_amount == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  *out_amount = channel->channel->buffered_amount();
  return RTC_OK;
}

RTC_API rtc_status RTC_CALL rtc_data_channel_close(rtc_data_channel* channel) {
  if (channel == nullptr || channel->channel == nullptr) {
    return RTC_ERR_INVALID_ARG;
  }
  channel->channel->Close();
  return RTC_OK;
}

RTC_API void RTC_CALL rtc_data_channel_release(rtc_data_channel* channel) {
  delete channel;
}
