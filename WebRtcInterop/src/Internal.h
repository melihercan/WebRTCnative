/*
 *  Shared between the interop translation units. Not part of the public
 *  surface: nothing here appears in include/Interop.h, and no C++ type
 *  declared here may cross the boundary.
 */

#ifndef WEBRTC_INTEROP_INTERNAL_H
#define WEBRTC_INTEROP_INTERNAL_H

#include <memory>
#include <mutex>

#include "Interop.h"
#include "api/audio/audio_device.h"
#include "api/data_channel_interface.h"
#include "api/rtp_sender_interface.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"

#if defined(_WIN32)
#include "rtc_base/win32_socket_init.h"
#endif

namespace webrtc_interop {

/* The three threads WebRTC needs. Owned by the library, created by
 * rtc_initialize and destroyed by rtc_terminate. Callbacks reach the caller
 * on the signalling thread. */
struct Runtime {
#if defined(_WIN32)
  /* Winsock must be started before any socket is created. Without it every
   * UDP socket fails with WSANOTINITIALISED, ICE gathers TCP placeholders
   * only, and the connection never leaves "checking". */
  webrtc::WinsockInitializer winsock;
#endif
  std::unique_ptr<webrtc::Thread> network_thread;
  std::unique_ptr<webrtc::Thread> worker_thread;
  std::unique_ptr<webrtc::Thread> signaling_thread;
};

extern std::mutex g_mutex;
extern std::unique_ptr<Runtime> g_runtime;

/* Copies into memory the caller frees with rtc_string_free. Returns null only
 * on allocation failure, which callers report as RTC_ERR_INTERNAL. */
char* DuplicateString(const char* value);

class InteropObserver;
class InteropDataChannelObserver;
class FrameSink;

}  // namespace webrtc_interop

/* -------------------------------------------------------------------------
 *  Handles
 *
 *  Defining the structs Interop.h forward-declares. The smart pointer lives
 *  here, on the heap, for exactly as long as the caller holds the handle.
 * ---------------------------------------------------------------------- */

struct rtc_factory {
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> ptr;
  /* The same instance the factory was built with, kept so device enumeration
   * reports the devices the engine will actually use. Null if the platform
   * refused to give us one, in which case the factory built its own and
   * enumeration reports RTC_ERR_INTERNAL. */
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm;
};

struct rtc_media_track {
  rtc_media_track();
  ~rtc_media_track();

  webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
  /* Audio tracks do not own their source, so hold a reference here to keep it
   * alive for as long as the caller holds the track. Video tracks do own
   * theirs, and this stays null. */
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source;
  /* Set while a frame sink is registered. Unregistered by the destructor, so
   * releasing a track with a live sink cannot leave a dangling registration. */
  std::unique_ptr<webrtc_interop::FrameSink> sink;
};

struct rtc_rtp_sender {
  webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender;
};

/* Transceivers and receivers are views, not owners: the peer connection owns
 * them, and get_transceivers hands back a fresh handle every time it is
 * called, so several handles may refer to one object. Releasing a handle drops
 * a reference and nothing more — it neither stops the transceiver nor
 * invalidates any other handle onto it. */
struct rtc_rtp_transceiver {
  webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver;
};

struct rtc_rtp_receiver {
  webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver;
};

struct rtc_data_channel {
  rtc_data_channel();
  ~rtc_data_channel();

  /* Declaration order is load-bearing here for the same reason as the peer
   * connection: the channel calls into its observer until it is gone. The
   * destructor also unregisters explicitly, because the channel is refcounted
   * by the peer connection and can outlive this handle. */
  std::unique_ptr<webrtc_interop::InteropDataChannelObserver> observer;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
};

struct rtc_peer_connection {
  rtc_peer_connection();
  ~rtc_peer_connection();

  /* Declaration order is load-bearing: members are destroyed in reverse, and
   * the peer connection calls into its observer right up until it is gone.
   * Observer declared first means peer connection destroyed first. */
  std::unique_ptr<webrtc_interop::InteropObserver> observer;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
};

#endif  /* WEBRTC_INTEROP_INTERNAL_H */
