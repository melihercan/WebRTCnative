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

}  // namespace webrtc_interop

/* -------------------------------------------------------------------------
 *  Handles
 *
 *  Defining the structs Interop.h forward-declares. The smart pointer lives
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
