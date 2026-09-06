/*
 *  WebRtcInterop — implementation of include/Interop.h.
 *
 *  Slice one, first cut: library lifecycle and the peer connection factory.
 *  The remaining declarations in the header are not implemented yet; adding
 *  one means adding it here and nowhere else.
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

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
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
 * on allocation failure, which callers report as RTC_ERR_INTERNAL. Kept for
 * the out-parameter strings the next slice adds. */
[[maybe_unused]] char* DuplicateString(const std::string& value) {
  char* copy = static_cast<char*>(std::malloc(value.size() + 1));
  if (copy == nullptr) {
    return nullptr;
  }
  std::memcpy(copy, value.c_str(), value.size() + 1);
  return copy;
}

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
