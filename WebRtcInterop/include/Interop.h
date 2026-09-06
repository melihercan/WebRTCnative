/*
 *  WebRtcInterop — a flat C surface over Google's WebRTC, for P/Invoke.
 *
 *  webrtc.dll is compiled with clang against libc++. No C++ type and no C++
 *  object may cross this boundary: an MSVC-compiled caller has a different
 *  std::string, allocator and exception model. Only int32_t, int64_t, double,
 *  pointers, UTF-8 const char* and function pointers appear below.
 *
 *  The full contract, including the threading rules, is the "Interop ABI" page
 *  of the WebRTCnative wiki. The two rules worth repeating here:
 *
 *    1. Every handle you receive — through an out-parameter or through a
 *       callback — is yours, and must be passed to exactly one matching
 *       _release. Nothing else transfers ownership.
 *
 *    2. Callbacks arrive on WebRTC's signalling thread. Do not block it.
 *       Copy what you need, hand it off, return.
 */

#ifndef WEBRTC_INTEROP_H
#define WEBRTC_INTEROP_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(RTC_INTEROP_IMPLEMENTATION)
#define RTC_API __declspec(dllexport)
#else
#define RTC_API __declspec(dllimport)
#endif
#define RTC_CALL __cdecl
#else
#define RTC_API __attribute__((visibility("default")))
#define RTC_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 *  Status
 * ---------------------------------------------------------------------- */

typedef int32_t rtc_status;

#define RTC_OK                 0
#define RTC_ERR_INVALID_ARG   -1
#define RTC_ERR_INVALID_STATE -2
#define RTC_ERR_NOT_FOUND     -3
#define RTC_ERR_UNSUPPORTED   -4
#define RTC_ERR_INTERNAL      -5

/* -------------------------------------------------------------------------
 *  Handles
 *
 *  Forward-declared rather than void*, so a mismatched argument is a compile
 *  error rather than a crash.
 * ---------------------------------------------------------------------- */

typedef struct rtc_factory rtc_factory;
typedef struct rtc_peer_connection rtc_peer_connection;
typedef struct rtc_media_track rtc_media_track;

/* -------------------------------------------------------------------------
 *  Enumerations
 *
 *  Fixed-width rather than C enums, whose size is implementation-defined and
 *  therefore unsafe to marshal. Values follow the W3C names.
 * ---------------------------------------------------------------------- */

typedef int32_t rtc_peer_connection_state;

#define RTC_PEER_CONNECTION_STATE_NEW          0
#define RTC_PEER_CONNECTION_STATE_CONNECTING   1
#define RTC_PEER_CONNECTION_STATE_CONNECTED    2
#define RTC_PEER_CONNECTION_STATE_DISCONNECTED 3
#define RTC_PEER_CONNECTION_STATE_FAILED       4
#define RTC_PEER_CONNECTION_STATE_CLOSED       5

typedef int32_t rtc_signaling_state;

#define RTC_SIGNALING_STATE_STABLE               0
#define RTC_SIGNALING_STATE_HAVE_LOCAL_OFFER     1
#define RTC_SIGNALING_STATE_HAVE_LOCAL_PRANSWER  2
#define RTC_SIGNALING_STATE_HAVE_REMOTE_OFFER    3
#define RTC_SIGNALING_STATE_HAVE_REMOTE_PRANSWER 4
#define RTC_SIGNALING_STATE_CLOSED               5

typedef int32_t rtc_media_kind;

#define RTC_MEDIA_KIND_AUDIO 0
#define RTC_MEDIA_KIND_VIDEO 1

/* W3C enumerateDevices separates "audioinput" from "audiooutput", so the
 * audio enumeration functions take which one is wanted. */
typedef int32_t rtc_audio_device_kind;

#define RTC_AUDIO_DEVICE_RECORDING 0 /* microphones  */
#define RTC_AUDIO_DEVICE_PLAYOUT   1 /* speakers     */

/* -------------------------------------------------------------------------
 *  Structures
 * ---------------------------------------------------------------------- */

typedef struct {
  const char* urls;     /* comma separated                     */
  const char* username; /* nullable                            */
  const char* password; /* nullable                            */
} rtc_ice_server;

typedef struct {
  const rtc_ice_server* ice_servers;
  int32_t ice_server_count;
} rtc_configuration;

/* An I420 frame. The planes belong to WebRTC and are valid only for the
 * duration of the rtc_on_frame_fn call. Copy or convert before returning. */
typedef struct {
  const uint8_t* y;
  const uint8_t* u;
  const uint8_t* v;
  int32_t stride_y;
  int32_t stride_u;
  int32_t stride_v;
  int32_t width;
  int32_t height;
  int64_t timestamp_us;
} rtc_video_frame;

/* -------------------------------------------------------------------------
 *  Callbacks
 *
 *  user_data is always first. Strings are borrowed for the duration of the
 *  call. Handles are owned by the receiver (rule 1).
 * ---------------------------------------------------------------------- */

typedef void(RTC_CALL* rtc_on_ice_candidate_fn)(void* user_data,
                                                const char* mid,
                                                int32_t mline_index,
                                                const char* sdp);

typedef void(RTC_CALL* rtc_on_connection_state_fn)(void* user_data,
                                                   rtc_peer_connection_state state);

typedef void(RTC_CALL* rtc_on_signaling_state_fn)(void* user_data,
                                                  rtc_signaling_state state);

/* track is owned by the receiver and must be released. */
typedef void(RTC_CALL* rtc_on_track_fn)(void* user_data,
                                        rtc_media_track* track,
                                        rtc_media_kind kind,
                                        const char* stream_id);

typedef void(RTC_CALL* rtc_on_renegotiation_needed_fn)(void* user_data);

typedef void(RTC_CALL* rtc_on_sdp_success_fn)(void* user_data,
                                              const char* type,
                                              const char* sdp);

typedef void(RTC_CALL* rtc_on_void_success_fn)(void* user_data);

typedef void(RTC_CALL* rtc_on_failure_fn)(void* user_data, const char* error);

typedef void(RTC_CALL* rtc_on_frame_fn)(void* user_data,
                                        const rtc_video_frame* frame);

/* Copied at registration; the caller need not keep it alive. Null members are
 * permitted and simply not raised. */
typedef struct {
  rtc_on_ice_candidate_fn on_ice_candidate;
  rtc_on_connection_state_fn on_connection_state;
  rtc_on_signaling_state_fn on_signaling_state;
  rtc_on_track_fn on_track;
  rtc_on_renegotiation_needed_fn on_renegotiation_needed;
} rtc_peer_connection_observer;

/* -------------------------------------------------------------------------
 *  Library
 * ---------------------------------------------------------------------- */

/* Creates WebRTC's global threads. Call once before anything else. */
RTC_API rtc_status RTC_CALL rtc_initialize(void);

/* Tears them down. No handle may be live. */
RTC_API rtc_status RTC_CALL rtc_terminate(void);

/* Frees any char* this library returned through an out-parameter. */
RTC_API void RTC_CALL rtc_string_free(char* s);

/* -------------------------------------------------------------------------
 *  Factory
 * ---------------------------------------------------------------------- */

/* Builtin audio and video encoder/decoder factories. Note that a standalone
 * WebRTC build has no H.264: VP8, VP9 and AV1 only. */
RTC_API rtc_status RTC_CALL rtc_factory_create(rtc_factory** out_factory);

RTC_API void RTC_CALL rtc_factory_release(rtc_factory* factory);

/* -------------------------------------------------------------------------
 *  Devices
 *
 *  Windows capture comes from modules/video_capture (DirectShow) and
 *  modules/audio_device (WASAPI), both present in webrtc.dll.
 *
 *  out_name and out_id are caller-owned; free with rtc_string_free.
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL rtc_video_device_count(rtc_factory* factory,
                                                   int32_t* out_count);

RTC_API rtc_status RTC_CALL rtc_video_device_info(rtc_factory* factory,
                                                  int32_t index,
                                                  char** out_name,
                                                  char** out_id);

RTC_API rtc_status RTC_CALL rtc_audio_device_count(rtc_factory* factory,
                                                   rtc_audio_device_kind kind,
                                                   int32_t* out_count);

RTC_API rtc_status RTC_CALL rtc_audio_device_info(rtc_factory* factory,
                                                  rtc_audio_device_kind kind,
                                                  int32_t index,
                                                  char** out_name,
                                                  char** out_id);

/* -------------------------------------------------------------------------
 *  Tracks
 *
 *  getUserMedia, taken apart: enumerate, then create a track from a chosen
 *  device. Constraint negotiation happens on the managed side, which then
 *  asks for concrete numbers.
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL rtc_audio_track_create(rtc_factory* factory,
                                                   const char* label,
                                                   rtc_media_track** out_track);

/* label becomes the track id, so keep it SDP-safe. Do not pass the device id:
 * a Windows device path contains backslashes and braces and would end up in
 * the msid attribute. */
RTC_API rtc_status RTC_CALL rtc_video_track_create(rtc_factory* factory,
                                                   const char* device_id,
                                                   const char* label,
                                                   int32_t width,
                                                   int32_t height,
                                                   int32_t fps,
                                                   rtc_media_track** out_track);

RTC_API rtc_status RTC_CALL rtc_media_track_set_enabled(rtc_media_track* track,
                                                        int32_t enabled);

/* out_id is caller-owned; free with rtc_string_free. */
RTC_API rtc_status RTC_CALL rtc_media_track_get_id(rtc_media_track* track,
                                                   char** out_id);

RTC_API void RTC_CALL rtc_media_track_release(rtc_media_track* track);

/* -------------------------------------------------------------------------
 *  Peer connection
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL
rtc_peer_connection_create(rtc_factory* factory,
                           const rtc_configuration* config,
                           const rtc_peer_connection_observer* observer,
                           void* user_data,
                           rtc_peer_connection** out_pc);

/* W3C close() — an observable state transition. The handle stays valid, so
 * callbacks already in flight can still land. Release separately. */
RTC_API rtc_status RTC_CALL rtc_peer_connection_close(rtc_peer_connection* pc);

RTC_API void RTC_CALL rtc_peer_connection_release(rtc_peer_connection* pc);

/* -------------------------------------------------------------------------
 *  Negotiation
 *
 *  These are asynchronous in WebRTC and stay asynchronous here: the return
 *  value reports only whether the request was accepted. Completion arrives on
 *  the callback, on the signalling thread.
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL
rtc_peer_connection_create_offer(rtc_peer_connection* pc,
                                 rtc_on_sdp_success_fn on_success,
                                 rtc_on_failure_fn on_failure,
                                 void* user_data);

RTC_API rtc_status RTC_CALL
rtc_peer_connection_create_answer(rtc_peer_connection* pc,
                                  rtc_on_sdp_success_fn on_success,
                                  rtc_on_failure_fn on_failure,
                                  void* user_data);

RTC_API rtc_status RTC_CALL
rtc_peer_connection_set_local_description(rtc_peer_connection* pc,
                                          const char* type,
                                          const char* sdp,
                                          rtc_on_void_success_fn on_success,
                                          rtc_on_failure_fn on_failure,
                                          void* user_data);

RTC_API rtc_status RTC_CALL
rtc_peer_connection_set_remote_description(rtc_peer_connection* pc,
                                           const char* type,
                                           const char* sdp,
                                           rtc_on_void_success_fn on_success,
                                           rtc_on_failure_fn on_failure,
                                           void* user_data);

RTC_API rtc_status RTC_CALL
rtc_peer_connection_add_ice_candidate(rtc_peer_connection* pc,
                                      const char* mid,
                                      int32_t mline_index,
                                      const char* sdp);

RTC_API rtc_status RTC_CALL
rtc_peer_connection_add_track(rtc_peer_connection* pc,
                              rtc_media_track* track,
                              const char* stream_id);

/* -------------------------------------------------------------------------
 *  Video frames
 *
 *  The frame sink runs at capture or decode rate on a WebRTC thread, and the
 *  planes die when the callback returns. Anything slower than the frame
 *  interval will drop frames or stall decoding.
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL rtc_video_track_add_sink(rtc_media_track* track,
                                                     rtc_on_frame_fn on_frame,
                                                     void* user_data);

RTC_API rtc_status RTC_CALL rtc_video_track_remove_sink(rtc_media_track* track);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WEBRTC_INTEROP_H */
