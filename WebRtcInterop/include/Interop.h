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
typedef struct rtc_data_channel rtc_data_channel;
typedef struct rtc_rtp_sender rtc_rtp_sender;
typedef struct rtc_rtp_transceiver rtc_rtp_transceiver;
typedef struct rtc_rtp_receiver rtc_rtp_receiver;

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

typedef int32_t rtc_data_channel_state;

#define RTC_DATA_CHANNEL_STATE_CONNECTING 0
#define RTC_DATA_CHANNEL_STATE_OPEN       1
#define RTC_DATA_CHANNEL_STATE_CLOSING    2
#define RTC_DATA_CHANNEL_STATE_CLOSED     3

/* getDisplayMedia separates a whole screen from a single window. */
typedef int32_t rtc_desktop_source_kind;

#define RTC_DESKTOP_SOURCE_SCREEN 0
#define RTC_DESKTOP_SOURCE_WINDOW 1

typedef int32_t rtc_media_kind;

#define RTC_MEDIA_KIND_AUDIO 0
#define RTC_MEDIA_KIND_VIDEO 1

/* W3C RTCRtpTransceiverDirection. "stopped" is reported but never accepted:
 * setDirection cannot stop a transceiver, rtc_rtp_transceiver_stop does. */
typedef int32_t rtc_rtp_transceiver_direction;

#define RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV 0
#define RTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY 1
#define RTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY 2
#define RTC_RTP_TRANSCEIVER_DIRECTION_INACTIVE 3
#define RTC_RTP_TRANSCEIVER_DIRECTION_STOPPED  4

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

/* W3C RTCDataChannelInit. The optional members are int32_t with -1 meaning
 * "not set" rather than pointers to values: it keeps the struct blittable, and
 * -1 is a value none of these fields can legitimately take. */
typedef struct {
  const char* protocol;         /* nullable                                 */
  int32_t ordered;              /* 0 or 1; 1 is the W3C default             */
  int32_t max_packet_life_time; /* milliseconds, or -1 for unset            */
  int32_t max_retransmits;      /* or -1 for unset                          */
  int32_t negotiated;           /* 0 or 1; 0 is the W3C default             */
  int32_t id;                   /* only meaningful when negotiated, else -1 */
} rtc_data_channel_init;

/* W3C RTCRtpEncodingParameters — one simulcast layer. Optional members use a
 * sentinel rather than a pointer, for the same reason rtc_data_channel_init
 * does: it keeps the struct blittable.
 *
 * rid names the layer in the SDP and is required as soon as there is more than
 * one encoding; a single unnamed encoding is the non-simulcast case. */
typedef struct {
  const char* rid;                 /* nullable; required when count > 1     */
  int32_t active;                  /* 0 or 1; 1 is the W3C default          */
  int32_t max_bitrate;             /* bits per second, or -1 for unset      */
  int32_t max_framerate;           /* or -1 for unset                       */
  /* 1.0 sends at capture resolution. 0 means unset, which WebRTC treats as
   * 1.0 — a legitimate value cannot be 0, since dividing by it is undefined. */
  double scale_resolution_down_by;
  const char* scalability_mode;    /* nullable, e.g. "L1T3"                 */
} rtc_rtp_encoding;

/* Clockwise degrees a renderer must turn the frame by before showing it.
 * The values are webrtc::VideoRotation's own, so the cast is an identity. */
typedef enum {
  RTC_VIDEO_ROTATION_0 = 0,
  RTC_VIDEO_ROTATION_90 = 90,
  RTC_VIDEO_ROTATION_180 = 180,
  RTC_VIDEO_ROTATION_270 = 270
} rtc_video_rotation;

/* An I420 frame. The planes belong to WebRTC and are valid only for the
 * duration of the rtc_on_frame_fn call. Copy or convert before returning.
 *
 * width and height describe the buffer, not the picture: a phone in portrait
 * sends 640x480 with rotation 90, and what a viewer should see is 480x640.
 * A consumer that ignores rotation therefore shows every phone on its side
 * and cannot detect the mistake, because the two cases are the same bytes at
 * the same dimensions. That is exactly what happened on Windows until
 * 2026-09-14: this struct carried no rotation, so the managed renderer had
 * nothing to apply. */
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
  rtc_video_rotation rotation;
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

/* A channel the peer opened. Owned by the receiver, like a track. */
typedef void(RTC_CALL* rtc_on_data_channel_fn)(void* user_data,
                                               rtc_data_channel* channel);

typedef void(RTC_CALL* rtc_on_data_channel_state_fn)(
    void* user_data,
    rtc_data_channel_state state);

/* data is borrowed for the duration of the call. is_binary separates a byte
 * payload from UTF-8 text: SCTP carries the distinction, and the W3C API
 * surfaces the two as different message types. */
typedef void(RTC_CALL* rtc_on_data_channel_message_fn)(void* user_data,
                                                       const uint8_t* data,
                                                       int32_t size,
                                                       int32_t is_binary);

typedef void(RTC_CALL* rtc_on_buffered_amount_change_fn)(void* user_data,
                                                         uint64_t buffered);

typedef void(RTC_CALL* rtc_on_sdp_success_fn)(void* user_data,
                                              const char* type,
                                              const char* sdp);

typedef void(RTC_CALL* rtc_on_void_success_fn)(void* user_data);

/* The report as JSON, borrowed for the duration of the call. See
 * rtc_peer_connection_get_stats for the shape. */
typedef void(RTC_CALL* rtc_on_stats_success_fn)(void* user_data,
                                                const char* json);

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
  /* Appended rather than inserted: this struct is passed by address and read
   * field by field, so a new member at the end leaves the existing ones where
   * they were. Callers built against the shorter struct still work. */
  rtc_on_data_channel_fn on_data_channel;
} rtc_peer_connection_observer;

/* Copied at registration, like the peer connection observer. Null members are
 * permitted and simply not raised. */
typedef struct {
  rtc_on_data_channel_state_fn on_state;
  rtc_on_data_channel_message_fn on_message;
  rtc_on_buffered_amount_change_fn on_buffered_amount_change;
} rtc_data_channel_observer;

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
 * the msid attribute.
 *
 * RTC_ERR_NOT_FOUND means no device matches device_id. RTC_ERR_INVALID_STATE
 * means the device is there but would not start, which is nearly always
 * another application holding the camera and occasionally a size or frame rate
 * it will not accept. The two are worth telling apart: the first is missing
 * hardware, the second is a camera someone else is using. */
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

/* Audio or video. A track the caller created carries a kind it already knows;
 * one reached through a receiver does not, having arrived through negotiation.
 * RTC_ERR_UNSUPPORTED for a kind that is neither, which this library cannot
 * produce. */
RTC_API rtc_status RTC_CALL rtc_media_track_get_kind(rtc_media_track* track,
                                                     rtc_media_kind* out_kind);

RTC_API void RTC_CALL rtc_media_track_release(rtc_media_track* track);

/* -------------------------------------------------------------------------
 *  Desktop capture
 *
 *  getDisplayMedia, taken apart the same way getUserMedia was: enumerate, then
 *  create a track from a chosen source. Enumeration takes no factory because a
 *  capturer needs none.
 *
 *  Windows uses the GDI capturers. The DirectX and Windows Graphics Capture
 *  paths are faster but require COM or WinRT to be initialised on the capture
 *  thread, which is a larger contract than this owes its caller.
 * ---------------------------------------------------------------------- */

RTC_API rtc_status RTC_CALL
rtc_desktop_source_count(rtc_desktop_source_kind kind, int32_t* out_count);

/* out_title is caller-owned; free with rtc_string_free. Screens often have no
 * title of their own, in which case a positional name is returned. out_id is
 * what rtc_desktop_track_create takes -- an index is not stable across calls. */
RTC_API rtc_status RTC_CALL
rtc_desktop_source_info(rtc_desktop_source_kind kind,
                        int32_t index,
                        char** out_title,
                        int64_t* out_id);

/* Capture starts before this returns, so an unusable source is reported here
 * rather than as a track that never produces a frame. */
RTC_API rtc_status RTC_CALL
rtc_desktop_track_create(rtc_factory* factory,
                         rtc_desktop_source_kind kind,
                         int64_t source_id,
                         const char* label,
                         int32_t max_fps,
                         rtc_media_track** out_track);

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

/* out_sender may be null when the caller does not intend to replace or remove
 * the track later; otherwise it receives a handle to release. */
RTC_API rtc_status RTC_CALL
rtc_peer_connection_add_track(rtc_peer_connection* pc,
                              rtc_media_track* track,
                              const char* stream_id,
                              rtc_rtp_sender** out_sender);

/* -------------------------------------------------------------------------
 *  Senders
 *
 *  What add_track hands back, so a track can be swapped or removed after the
 *  fact. There is deliberately no get_senders: the caller already knows what
 *  it added, and a list function would hand back handles whose ownership is
 *  ambiguous.
 * ---------------------------------------------------------------------- */

/* W3C replaceTrack. Passing a null track stops sending without renegotiating,
 * which is how a camera is muted at the transport rather than the source. The
 * new track must be the same kind as the old one. */
RTC_API rtc_status RTC_CALL
rtc_rtp_sender_replace_track(rtc_rtp_sender* sender, rtc_media_track* track);

/* W3C getParameters / setParameters, narrowed to the encodings, which are the
 * part a caller can change.
 *
 * The transaction deliberately does not cross the ABI. WebRTC rejects a
 * setParameters whose argument did not come from a recent getParameters on the
 * same sender -- the parameters carry a transaction id, and a stale one fails
 * at run time with InvalidModification, not at compile time -- so
 * set_parameters does its own get, applies the fields below, and sets the
 * result back. The caller therefore never holds a token it has to keep fresh.
 *
 * Only these encoding fields are written by a set: active, max_bitrate,
 * max_framerate, scale_resolution_down_by and scalability_mode. rid is left as
 * negotiation settled it, because WebRTC rejects a change to it, and
 * everything else an encoding carries is read-only.
 *
 * Note which side enforces what, because it is not uniform and the difference
 * is visible in the stats. max_bitrate is enforced by the encoder.
 * scale_resolution_down_by is enforced by the encoder too -- it scales a frame
 * that does not match the configured resolution -- so the picture shrinks even
 * for a source that ignores adaptation, which is why outbound-rtp frameWidth
 * is no evidence about the source. max_framerate is the one the SOURCE's frame
 * adapter has to apply: measured on M152, a source that does not call
 * AdaptFrame keeps delivering at capture rate and the request is simply lost.
 * This library's camera and desktop sources do call it.
 */

/* Two-call, like get_transceivers: pass a null buffer to learn the count, then
 * a buffer of at least that size. Returns RTC_ERR_INVALID_ARG if the buffer is
 * too small, having written nothing.
 *
 * rid and scalability_mode are caller-owned on return and null when unset:
 * free each non-null one with rtc_string_free. The numeric fields use the
 * sentinels rtc_rtp_encoding documents, so what comes out of a get can be fed
 * straight back into a set. */
RTC_API rtc_status RTC_CALL
rtc_rtp_sender_get_parameters(rtc_rtp_sender* sender,
                              rtc_rtp_encoding* buffer,
                              int32_t capacity,
                              int32_t* out_count);

/* encoding_count must equal the number of encodings the sender already has --
 * WebRTC does not allow the count to change -- which is what get_parameters
 * reports. RTC_ERR_INVALID_ARG if it differs, or if the sender refuses a
 * value; RTC_ERR_INVALID_STATE if the peer connection is closed. */
RTC_API rtc_status RTC_CALL
rtc_rtp_sender_set_parameters(rtc_rtp_sender* sender,
                              const rtc_rtp_encoding* encodings,
                              int32_t encoding_count);

/* The sender handle stays valid and must still be released. */
RTC_API rtc_status RTC_CALL
rtc_peer_connection_remove_track(rtc_peer_connection* pc,
                                 rtc_rtp_sender* sender);

RTC_API void RTC_CALL rtc_rtp_sender_release(rtc_rtp_sender* sender);

/* -------------------------------------------------------------------------
 *  Transceivers
 *
 *  The unified-plan view of a peer connection: one transceiver per m-section,
 *  each pairing a sender with a receiver. The peer connection is already
 *  unified plan, so these have always existed underneath — they were simply
 *  not exported, which left addTrack as the only way to send and made this
 *  binding unusable by anything that negotiates per m-section. A client that
 *  has to read a "mid", ask for specific simulcast encodings, or match an
 *  incoming stream to the section that carries it needs these; mediasoup does
 *  all three.
 *
 *  Unlike senders there IS a list function here, and it has to exist: the
 *  interesting transceivers are the ones the REMOTE end created, which arrive
 *  through negotiation rather than through any call the caller made. Its
 *  ownership rule is the usual one — every handle it writes is yours, and each
 *  needs its own release. Handles are views onto the same underlying object,
 *  so fetching the list twice gives two sets of handles for the same
 *  transceivers, and releasing one set does not disturb the other.
 * ---------------------------------------------------------------------- */

/* addTransceiver. Pass a track to send it, or null to add a transceiver of
 * kind alone — which is how a client discovers what the platform can encode,
 * by adding one of each kind and reading the offer it generates.
 *
 * kind is ignored when track is non-null; the track's own kind wins.
 *
 * stream_ids may be null, and normally is: mediasoup identifies streams by
 * mid, not by msid. encodings may be null for a single default encoding, or
 * point to encoding_count layers for simulcast. */
RTC_API rtc_status RTC_CALL
rtc_peer_connection_add_transceiver(rtc_peer_connection* pc,
                                    rtc_media_kind kind,
                                    rtc_media_track* track,
                                    rtc_rtp_transceiver_direction direction,
                                    const char* const* stream_ids,
                                    int32_t stream_id_count,
                                    const rtc_rtp_encoding* encodings,
                                    int32_t encoding_count,
                                    rtc_rtp_transceiver** out_transceiver);

/* getTransceivers, as a two-call: pass a null buffer to learn the count, then
 * a buffer of at least that size. Returns RTC_ERR_INVALID_ARG if the buffer is
 * too small, having written nothing, so a caller that raced a renegotiation
 * can simply ask again rather than free a half-filled array.
 *
 * out_count is always written when it is non-null, including on the counting
 * call and including when the count is zero. */
RTC_API rtc_status RTC_CALL
rtc_peer_connection_get_transceivers(rtc_peer_connection* pc,
                                     rtc_rtp_transceiver** buffer,
                                     int32_t capacity,
                                     int32_t* out_count);

/* The m-section identifier this transceiver negotiated. Null until the local
 * description that names it has been applied, which is reported as
 * RTC_ERR_NOT_FOUND rather than as an empty string — the distinction matters,
 * because reading it too early is the ordinary mistake here and an empty
 * string would look like a valid answer. out_mid is caller-owned; free with
 * rtc_string_free. */
RTC_API rtc_status RTC_CALL
rtc_rtp_transceiver_get_mid(rtc_rtp_transceiver* transceiver, char** out_mid);

RTC_API rtc_status RTC_CALL rtc_rtp_transceiver_get_direction(
    rtc_rtp_transceiver* transceiver,
    rtc_rtp_transceiver_direction* out_direction);

/* What the negotiation actually settled on, which is not what was asked for
 * until an answer has been exchanged. RTC_ERR_NOT_FOUND while unset. */
RTC_API rtc_status RTC_CALL rtc_rtp_transceiver_get_current_direction(
    rtc_rtp_transceiver* transceiver,
    rtc_rtp_transceiver_direction* out_direction);

RTC_API rtc_status RTC_CALL rtc_rtp_transceiver_set_direction(
    rtc_rtp_transceiver* transceiver,
    rtc_rtp_transceiver_direction direction);

/* Both hand back a new handle, which is yours to release. */
RTC_API rtc_status RTC_CALL
rtc_rtp_transceiver_get_sender(rtc_rtp_transceiver* transceiver,
                               rtc_rtp_sender** out_sender);

RTC_API rtc_status RTC_CALL
rtc_rtp_transceiver_get_receiver(rtc_rtp_transceiver* transceiver,
                                 rtc_rtp_receiver** out_receiver);

/* W3C stop(). The handle stays valid and must still be released. */
RTC_API rtc_status RTC_CALL
rtc_rtp_transceiver_stop(rtc_rtp_transceiver* transceiver);

RTC_API void RTC_CALL
rtc_rtp_transceiver_release(rtc_rtp_transceiver* transceiver);

/* -------------------------------------------------------------------------
 *  Receivers
 * ---------------------------------------------------------------------- */

/* The remote track this receiver delivers. Present as soon as the transceiver
 * exists, and before any media arrives. The handle is yours to release. */
RTC_API rtc_status RTC_CALL
rtc_rtp_receiver_get_track(rtc_rtp_receiver* receiver,
                           rtc_media_track** out_track);

RTC_API void RTC_CALL rtc_rtp_receiver_release(rtc_rtp_receiver* receiver);

/* -------------------------------------------------------------------------
 *  Data channels
 *
 *  An SCTP channel alongside the media. Creating one before the offer puts an
 *  m=application section in the SDP; creating one afterwards raises
 *  on_renegotiation_needed, exactly as the W3C API does.
 * ---------------------------------------------------------------------- */

/* init may be null, which takes the W3C defaults: ordered, reliable, not
 * negotiated. */
RTC_API rtc_status RTC_CALL
rtc_peer_connection_create_data_channel(rtc_peer_connection* pc,
                                        const char* label,
                                        const rtc_data_channel_init* init,
                                        rtc_data_channel** out_channel);

/* Copied at registration; null clears it. Register before the channel opens or
 * the open transition is missed -- for a channel arriving through
 * on_data_channel that means registering inside the callback. */
RTC_API rtc_status RTC_CALL
rtc_data_channel_set_observer(rtc_data_channel* channel,
                              const rtc_data_channel_observer* observer,
                              void* user_data);

/* Fails with RTC_ERR_INVALID_STATE unless the channel is open. */
RTC_API rtc_status RTC_CALL rtc_data_channel_send(rtc_data_channel* channel,
                                                  const uint8_t* data,
                                                  int32_t size,
                                                  int32_t is_binary);

/* out_label is caller-owned; free with rtc_string_free. */
RTC_API rtc_status RTC_CALL
rtc_data_channel_get_label(rtc_data_channel* channel, char** out_label);

/* -1 until the channel is negotiated and the transport has assigned one. */
RTC_API rtc_status RTC_CALL rtc_data_channel_get_id(rtc_data_channel* channel,
                                                    int32_t* out_id);

RTC_API rtc_status RTC_CALL
rtc_data_channel_get_state(rtc_data_channel* channel,
                           rtc_data_channel_state* out_state);

RTC_API rtc_status RTC_CALL
rtc_data_channel_get_buffered_amount(rtc_data_channel* channel,
                                     uint64_t* out_amount);

RTC_API rtc_status RTC_CALL rtc_data_channel_close(rtc_data_channel* channel);

RTC_API void RTC_CALL rtc_data_channel_release(rtc_data_channel* channel);

/* -------------------------------------------------------------------------
 *  Statistics
 *
 *  W3C getStats. The report is a heterogeneous bag -- every stats type has a
 *  different member set, defined by a specification that versions separately
 *  from this one -- so it crosses as JSON rather than as a walk over handles.
 *  Modelling it structurally would mean an attribute type enum and an
 *  accessor per type, and would still need revisiting whenever WebRTC adds a
 *  member. WebRTC already serialises the report itself, so this is a copy
 *  rather than a translation.
 * ---------------------------------------------------------------------- */

/* Asynchronous, like the negotiation calls: the return value reports only
 * that collection was started, and the report arrives on on_success on the
 * signalling thread.
 *
 * The JSON is an array of stats objects, each carrying "id", "type" and
 * "timestamp" alongside the members of its type. Timestamps are
 * MICROSECONDS, as WebRTC reports them, not the milliseconds W3C
 * DOMHighResTimeStamp uses -- converting is left to the caller, which knows
 * which of the two it wants.
 *
 * on_failure is raised only if the report cannot be serialised; WebRTC's own
 * collection has no failure path, and a closed peer connection yields an
 * empty report rather than an error. */
RTC_API rtc_status RTC_CALL
rtc_peer_connection_get_stats(rtc_peer_connection* pc,
                              rtc_on_stats_success_fn on_success,
                              rtc_on_failure_fn on_failure,
                              void* user_data);

/* The same report narrowed to one sender or one receiver — W3C getStats() on
 * RTCRtpSender / RTCRtpReceiver. The peer connection is passed because it, not
 * the sender, owns stats collection.
 *
 * These take a selector, which the connection-wide call deliberately does not:
 * there, "the whole report" is the only sensible answer, whereas a client
 * consuming several remote tracks needs to attribute inbound statistics to the
 * track they belong to, and the report gives it no way to do that afterwards.
 *
 * RTC_ERR_INVALID_ARG if the selector does not belong to this peer
 * connection — a mistake that would otherwise surface as a silently empty
 * report. */
RTC_API rtc_status RTC_CALL
rtc_rtp_sender_get_stats(rtc_peer_connection* pc,
                         rtc_rtp_sender* sender,
                         rtc_on_stats_success_fn on_success,
                         rtc_on_failure_fn on_failure,
                         void* user_data);

RTC_API rtc_status RTC_CALL
rtc_rtp_receiver_get_stats(rtc_peer_connection* pc,
                           rtc_rtp_receiver* receiver,
                           rtc_on_stats_success_fn on_success,
                           rtc_on_failure_fn on_failure,
                           void* user_data);

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
