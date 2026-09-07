# Interop ABI

The contract between `WebRtcInterop.dll` and `WebRTCme.Bindings.Maui.Windows`.

This is the layer Android and iOS get for free. Their bindings translate a Java or Objective-C SDK
that Google already wrote; Windows has no such SDK, so it has to be built here. See
[Platform layers](Platform-layers) for why.

Two repositories, one contract:

```
WebRTCnative                                    WebRTCme
  webrtc.dll   (Google's tree, clang + libc++)
  WebRtcInterop.dll  ── flat C exports ──►  P/Invoke ──► Bindings.Maui.Windows ──► WebRTCme.Api
```

Changing this ABI means changing both sides together, so it is worth getting the conventions right
before writing the second function.

## Why it has to be C, and why it lives here

`webrtc.dll` is compiled with clang against **libc++** — the component build ships `libc++.dll`
beside it. Anything MSVC compiles has a different `std::string`, a different allocator and a
different exception model, so no C++ type and no C++ object may cross the boundary. That rules out
C++/CLI, and it rules out linking WebRTC's C++ headers from a Visual Studio project.

The consequence: the shim must be compiled by the *same* clang toolchain, inside the GN build. It
is grafted into the checkout at `src/WebRtcInterop` and built by
[WebRtcNativeInteropWindows](Workflow-reference). It cannot live in WebRTCme.

Only these cross the boundary: `int32_t`, `int64_t`, `double`, pointers, `const char*` holding
UTF-8, and function pointers.

## Conventions

### Naming

`rtc_<object>_<verb>`, lower snake case. Creation is `rtc_<object>_create`, disposal is
`rtc_<object>_release`. Everything is `extern "C"`, exported with `RTC_API` and declared `RTC_CALL` (`__cdecl` on
Windows, so x86 works too).

### Handles

Every object crosses as an opaque, forward-declared pointer — never `void*`, so the compiler still
catches mismatches:

```c
typedef struct rtc_factory          rtc_factory;
typedef struct rtc_peer_connection  rtc_peer_connection;
typedef struct rtc_media_track      rtc_media_track;
```

On the C# side these are `IntPtr` wrapped in a `SafeHandle` per type.

### Ownership — the single rule

> **Every handle you receive — through an out-parameter or through a callback — is yours, and must
> be passed to exactly one matching `_release`.**

Nothing else transfers ownership. No handle is freed as a side effect of another call, and
`_release` on a handle you did not receive is undefined.

The callback half of that rule exists for `on_track`, which delivers a `rtc_media_track*` the
receiver must release. Handles are the one thing a callback hands over rather than lends; strings
passed to a callback stay borrowed.

This rule exists because the 2023 attempt died without one: `CreateBuiltinAudioEncoderFactory`
returned the address of a stack `scoped_refptr`, and `CallCreatePeerConnectionFactory` was
abandoned because there was no answer to who owned the result. Internally the shim keeps WebRTC's
`scoped_refptr` alive in a heap struct; the handle is a pointer to that struct, and `_release`
destroys it. Reference counting stays entirely on the C++ side.

### Errors

Every function returns a status. Results come back through out-parameters.

```c
typedef int32_t rtc_status;

#define RTC_OK                 0
#define RTC_ERR_INVALID_ARG   -1
#define RTC_ERR_INVALID_STATE -2
#define RTC_ERR_NOT_FOUND     -3
#define RTC_ERR_UNSUPPORTED   -4
#define RTC_ERR_INTERNAL      -5
```

**The shim is written exception-free, not exception-guarded.** WebRTC compiles with
`-fno-exceptions`, and so does anything grafted into its build — a `try` block is a compile error:

```
error: cannot use 'try' with exceptions disabled
```

That is not a restriction to work around. With exceptions disabled nothing in the process can
throw, so there is nothing to catch: an exception cannot unwind into P/Invoke because one cannot
arise. Allocation that may fail uses `new (std::nothrow)` and is checked, returning
`RTC_ERR_INTERNAL`.

An earlier draft of this page prescribed wrapping every entry point in `try/catch(...)`. That was
written before the first build and is wrong.

### Strings

UTF-8, always. Strings *into* the shim are borrowed for the duration of the call — the shim copies
what it needs. Strings *out* of the shim are heap-allocated and owned by the caller:

```c
void rtc_string_free(char* s);
```

SDP blobs are the reason for allocate-and-free rather than a caller buffer: they are large,
variable, and a two-call length probe doubles the P/Invoke count on the hottest path.

```csharp
var sdp = Marshal.PtrToStringUTF8(sdpPtr);
NativeMethods.rtc_string_free(sdpPtr);
```

### Callbacks

A function pointer plus a `void* user_data`, always in that order, `user_data` first in the
signature:

```c
typedef void (*rtc_on_ice_candidate_fn)(void* user_data,
                                        const char* mid,
                                        int32_t mline_index,
                                        const char* sdp);
```

Strings passed *to* a callback are owned by the shim and valid only for the duration of the call.
Copy before returning.

On the C# side these are `[UnmanagedCallersOnly]` static methods, never delegates — a delegate
needs pinning and is the classic source of "worked in debug, crashed in release". Instance context
travels as a `GCHandle`:

```csharp
[UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
static void OnIceCandidate(IntPtr ctx, IntPtr mid, int mline, IntPtr sdp)
{
    var self = (PeerConnection)GCHandle.FromIntPtr(ctx).Target!;
    self.RaiseIceCandidate(Marshal.PtrToStringUTF8(mid), mline, Marshal.PtrToStringUTF8(sdp));
}
```

### Winsock, on Windows

`rtc_initialize` holds a `WinsockInitializer` for the library's lifetime. Without it every UDP
socket fails with `WSANOTINITIALISED`, and the failure is quiet in a way that wastes an afternoon:
ICE still gathers TCP placeholder candidates, `addIceCandidate` still returns success, the
connection still reaches `checking` — and then never leaves it.

Set `WEBRTC_INTEROP_VERBOSE` to route WebRTC's own logging to stderr. The ICE layer is silent
otherwise, and that variable is how this was found.

### Threading — read this before writing a handler

**Callbacks arrive on WebRTC's signalling thread, not yours.** That thread must not be blocked;
stalling it stalls the whole connection.

So a handler does one thing: marshal the data into managed memory and hand it off. `WebRTCme.Middleware`
is Rx-based, so the natural landing point is `subject.OnNext(...)` with observation moved to the UI
scheduler downstream. No `await`, no locks held across the call, no dispatcher round-trip inside
the handler.

Reentrancy: calling back into the shim from inside a callback is allowed for the state-free calls
(`rtc_string_free`, getters), and undefined for anything that mutates the peer connection. Queue
those.

### Observers

A peer connection needs many callbacks, so they are registered as one struct of function pointers
at creation time. It is blittable, so C# declares it with `IntPtr` fields holding
`&Method` addresses.

```c
typedef struct {
  rtc_on_ice_candidate_fn          on_ice_candidate;
  rtc_on_connection_state_fn       on_connection_state;
  rtc_on_signaling_state_fn        on_signaling_state;
  rtc_on_track_fn                  on_track;
  rtc_on_renegotiation_needed_fn   on_renegotiation_needed;
} rtc_peer_connection_observer;
```

Null members are permitted and simply not raised. The struct is copied at registration; the caller
need not keep it alive.

## Verified layout

Every struct is blittable on x64 — no `[MarshalAs]`, no custom marshaller, no packing attribute.
Confirmed by compiling the header with the same `clang-cl` that builds `webrtc.dll`
(clang 23, `x86_64-pc-windows-msvc`):

| Type | Size | Field offsets |
|---|---|---|
| `rtc_status` | 4 | — |
| `rtc_peer_connection_state` | 4 | — |
| `rtc_ice_server` | 24 | `urls` 0, `username` 8, `password` 16 |
| `rtc_configuration` | 16 | `ice_servers` 0, `ice_server_count` 8 |
| `rtc_video_frame` | 56 | `y` 0, `width` 36, `timestamp_us` 48 |
| `rtc_peer_connection_observer` | 48 | six function pointers, `on_data_channel` at 40 |
| `rtc_data_channel_init` | 32 | `protocol` 0, `ordered` 8, `max_packet_life_time` 12, `max_retransmits` 16, `negotiated` 20, `id` 24 |
| `rtc_data_channel_observer` | 24 | three function pointers |

The header also compiles clean as C11 and as C++17 under `/W4`, so it can be consumed by a C
caller, a C++ caller, or read as documentation without a toolchain.

## Slice one

The smallest surface that carries an audio and video call between two Windows peers. Twenty-five
functions, declared in `WebRtcInterop/include/Interop.h`.

**All twenty-five are implemented**, and data channels have since been added on top — see below.
Six tests in `WebRtcInterop/test/` cover the surface: `Handshake.c` drives a full offer/answer/ICE
exchange between two peer connections, `FrameSink.c` opens a camera and checks the delivered
frames, `Devices.c` enumerates every device kind and exercises the error paths, and
`DataChannel.c` opens a channel across a handshake and sends both ways, and `Sender.c`
replaces and removes a track on a live sender, and
`DesktopCapture.c` enumerates shareable sources and checks captured frames.

### Library — **implemented**

```c
rtc_status rtc_initialize(void);
rtc_status rtc_terminate(void);
void       rtc_string_free(char* s);
```

`rtc_initialize` calls `webrtc::InitializeSSL` and starts the network, worker and signalling
threads; `rtc_terminate` stops them in reverse and cleans up SSL. Call once each, at assembly load
and unload. Calling either twice returns `RTC_ERR_INVALID_STATE`.

### Factory — **implemented**

```c
rtc_status rtc_factory_create(rtc_factory** out_factory);
void       rtc_factory_release(rtc_factory* factory);
```

Before `rtc_initialize` it returns `RTC_ERR_INVALID_STATE`; with a null out-parameter,
`RTC_ERR_INVALID_ARG`. `rtc_factory_release(NULL)` is a no-op, so a failed create needs no special
case.

Wraps `CreatePeerConnectionFactory` with the builtin audio and video encoder and decoder factories.
Note that on Windows the builtin video factory means **VP8, VP9 and AV1 only** — there is no H.264
in a standalone build. See [Platform layers](Platform-layers).

### Devices — **implemented**

```c
rtc_status rtc_video_device_count(rtc_factory* f, int32_t* out_count);
rtc_status rtc_video_device_info(rtc_factory* f, int32_t index,
                                 char** out_name, char** out_id);
rtc_status rtc_audio_device_count(rtc_factory* f, rtc_audio_device_kind kind,
                                  int32_t* out_count);
rtc_status rtc_audio_device_info(rtc_factory* f, rtc_audio_device_kind kind,
                                 int32_t index,
                                 char** out_name, char** out_id);
```

Video is backed by `modules/video_capture` (DirectShow on Windows). Output strings are
caller-owned; an out-of-range index returns `RTC_ERR_NOT_FOUND`.

`kind` is `RTC_AUDIO_DEVICE_RECORDING` or `RTC_AUDIO_DEVICE_PLAYOUT`, mirroring W3C's
`audioinput` / `audiooutput` split.

Audio needs a live `AudioDeviceModule`, so `rtc_factory_create` builds one with
`CreateAudioDeviceModule` and hands that same instance to the peer connection factory. Enumeration
then reports the devices the engine will actually use, rather than a second module's view of them.

The module has thread affinity to the worker thread, so it is created there and every call hops
back with `BlockingCall`. A machine with no usable audio device is not fatal: the factory falls
back to building its own, and enumeration reports `RTC_ERR_INTERNAL`.

Some drivers report an empty endpoint GUID, so `out_id` falls back to the name — the caller always
has something to select with.

### Tracks — **implemented**

```c
rtc_status rtc_audio_track_create(rtc_factory* f, const char* label,
                                  rtc_media_track** out_track);
rtc_status rtc_video_track_create(rtc_factory* f, const char* device_id,
                                  const char* label,
                                  int32_t width, int32_t height, int32_t fps,
                                  rtc_media_track** out_track);
rtc_status rtc_media_track_set_enabled(rtc_media_track* t, int32_t enabled);
rtc_status rtc_media_track_get_id(rtc_media_track* t, char** out_id);
void       rtc_media_track_release(rtc_media_track* t);
```

This is `getUserMedia` reduced to its parts: enumerate, then create a track from a chosen device.
The constraint negotiation `WebRTCme.Api` exposes is resolved on the C# side, which then asks for
concrete numbers.

`label` becomes the track id and must be SDP-safe. It exists because the first implementation
passed the device id and produced a track whose id was a Windows device path — backslashes, braces
and all — headed for the `msid` attribute.

Video capture has no ready-made source upstream: `modules/video_capture` produces frames through a
`VideoSinkInterface` while a track needs a `VideoTrackSourceInterface`, so the shim carries a small
`CameraSource` bridging the two through `AdaptedVideoTrackSource`. The camera opens when the track
is created and closes when the last reference to it goes away.

### Peer connection — **implemented**

```c
typedef struct {
  const char* urls;      // comma separated
  const char* username;  // nullable
  const char* password;  // nullable
} rtc_ice_server;

typedef struct {
  const rtc_ice_server* ice_servers;
  int32_t               ice_server_count;
} rtc_configuration;

rtc_status rtc_peer_connection_create(rtc_factory* f,
                                      const rtc_configuration* config,
                                      const rtc_peer_connection_observer* observer,
                                      void* user_data,
                                      rtc_peer_connection** out_pc);
rtc_status rtc_peer_connection_close(rtc_peer_connection* pc);
void       rtc_peer_connection_release(rtc_peer_connection* pc);
```

`close` is separate from `release`: W3C `close()` is an observable state transition, and the handle
must stay valid for callbacks still in flight.

### Negotiation — **implemented**

```c
typedef void (*rtc_on_sdp_success_fn)(void* user_data, const char* type, const char* sdp);
typedef void (*rtc_on_sdp_failure_fn)(void* user_data, const char* error);
typedef void (*rtc_on_void_success_fn)(void* user_data);

rtc_status rtc_peer_connection_create_offer(rtc_peer_connection* pc,
                                            rtc_on_sdp_success_fn on_success,
                                            rtc_on_sdp_failure_fn on_failure,
                                            void* user_data);
rtc_status rtc_peer_connection_create_answer(rtc_peer_connection* pc,
                                             rtc_on_sdp_success_fn on_success,
                                             rtc_on_sdp_failure_fn on_failure,
                                             void* user_data);
rtc_status rtc_peer_connection_set_local_description(rtc_peer_connection* pc,
                                                     const char* type, const char* sdp,
                                                     rtc_on_void_success_fn on_success,
                                                     rtc_on_sdp_failure_fn on_failure,
                                                     void* user_data);
rtc_status rtc_peer_connection_set_remote_description(rtc_peer_connection* pc,
                                                      const char* type, const char* sdp,
                                                      rtc_on_void_success_fn on_success,
                                                      rtc_on_sdp_failure_fn on_failure,
                                                      void* user_data);
rtc_status rtc_peer_connection_add_ice_candidate(rtc_peer_connection* pc,
                                                 const char* mid, int32_t mline_index,
                                                 const char* sdp);
rtc_status rtc_peer_connection_add_track(rtc_peer_connection* pc,
                                         rtc_media_track* track,
                                         const char* stream_id);
```

These four are asynchronous in WebRTC and stay asynchronous here — the return value only reports
whether the request was *accepted*. The C# side turns each into a `TaskCompletionSource` so
`WebRTCme.Api` can present the `Task` shape it already has.

### Video frames — **implemented**

```c
typedef struct {
  const uint8_t* y; int32_t stride_y;
  const uint8_t* u; int32_t stride_u;
  const uint8_t* v; int32_t stride_v;
  int32_t width, height;
  int64_t timestamp_us;
} rtc_video_frame;

typedef void (*rtc_on_frame_fn)(void* user_data, const rtc_video_frame* frame);

rtc_status rtc_video_track_add_sink(rtc_media_track* t,
                                    rtc_on_frame_fn on_frame, void* user_data);
rtc_status rtc_video_track_remove_sink(rtc_media_track* t);
```

Frames arrive on a WebRTC capture or decode thread and the planes belong to WebRTC for exactly the
duration of the callback, so the handler must copy or convert and return. Anything slower than the
frame interval drops frames or stalls decoding.

The shim calls `ToI420()` on the incoming buffer: a no-op when the frame already is I420, which is
the common case for a camera, and a conversion otherwise. The returned reference is held for the
callback, which is what keeps the planes alive.

One sink per track. Adding a second returns `RTC_ERR_INVALID_STATE` rather than silently replacing
the first and leaking its registration, and adding one to an audio track returns
`RTC_ERR_INVALID_ARG`. `RemoveSink` is synchronous, so once `rtc_video_track_remove_sink` returns
no further callback can be in flight. Releasing a track that still has a sink unregisters it in the
destructor rather than leaving WebRTC holding a pointer into freed memory.

Measured against a real camera: 85 frames in 2.80 s — 30.4 fps against a requested 30 — at
640x480 with strides y=640, u=320, v=320, no null planes and no blank rows.

## Data channels — **implemented**

Nine functions, added after slice one and following the same conventions.

```c
rtc_peer_connection_create_data_channel(pc, label, init, &channel);
rtc_data_channel_set_observer(channel, &observer, user_data);
rtc_data_channel_send(channel, data, size, is_binary);
rtc_data_channel_get_label(channel, &label);       /* caller frees   */
rtc_data_channel_get_id(channel, &id);             /* -1 until open  */
rtc_data_channel_get_state(channel, &state);
rtc_data_channel_get_buffered_amount(channel, &amount);
rtc_data_channel_close(channel);
rtc_data_channel_release(channel);
```

Create the channel **before** the offer and it rides along as an `m=application` section; create
it afterwards and `on_renegotiation_needed` fires, exactly as in the W3C API.

Three decisions are worth knowing before adding to this area.

**The observer is registered separately, not passed to the create call.** A channel that arrives
through `on_data_channel` does not exist until that callback runs, so there is nowhere to have
passed an observer. Making registration a separate step gives both cases one shape — and it means
an incoming channel *must* be observed from inside the callback, because the open transition can
follow immediately and is otherwise missed.

**`rtc_data_channel_init` carries its optional members as `int32_t` with `-1` for unset**, rather
than pointers to values. It keeps the struct blittable for P/Invoke, and `-1` is not a value any of
`max_packet_life_time`, `max_retransmits` or `id` can legitimately take.

**`on_data_channel` was appended to `rtc_peer_connection_observer`, not inserted.** The struct is
passed by address and read field by field, so the five original members keep their offsets and a
caller compiled against the shorter struct still works. Anything added here later must go on the
end for the same reason.

Sending is asynchronous. The status reports that the channel was open and the payload was
accepted — not that it was delivered — which matches `rtc_peer_connection_create_offer` here and
the W3C `send()`, and avoids WebRTC's own `Send()` bool that its header documents as unreliable.
Failures after acceptance are logged, not surfaced.

The handle unregisters its observer explicitly in its destructor rather than relying on member
destruction order, because the peer connection holds its own reference and the channel can outlive
the handle.

`test/DataChannel.c` covers it against a real handshake: `m=application` in the offer, both ends
reaching open with matching ids, text and binary each way with the binary flag intact, the error
paths, and release with a live observer still registered.

## Senders — **implemented**

`add_track` gained an out-parameter and three functions joined it.

```c
rtc_peer_connection_add_track(pc, track, stream_id, &sender);  /* sender may be null */
rtc_rtp_sender_replace_track(sender, track);                   /* track may be null  */
rtc_peer_connection_remove_track(pc, sender);
rtc_rtp_sender_release(sender);
```

**This changed an existing signature**, unlike the data channel slice which only appended. A
caller built against the four-argument `add_track` will not work against the three-argument one
and vice versa, so the shim and the binding have to move together. That is acceptable here
because they ship as a pair, but it is the reason the observer struct got a new member on the end
rather than the same treatment.

`out_sender` is nullable, and that is the normal case: a caller that will never replace or remove
the track passes null and has no handle to release. The track is added either way.

**`replace_track` is the point of the slice.** It swaps what a sender transmits *without*
renegotiating — no new offer, no interruption, the connection stays up. Doing the same thing as
remove-then-add would force a fresh offer/answer round and a visible gap. A null track is
meaningful rather than an error: it stops the sender while leaving the transport in place, which
is how muting is done at the sender rather than at the source.

**`remove_track` is idempotent**, which surprised the test before it surprised anyone else. W3C
`removeTrack` aborts quietly when the sender's track is already null, so a second removal returns
`RTC_OK` rather than an error. The test originally asserted `RTC_ERR_INVALID_STATE` and was wrong.

There is deliberately **no `get_senders`**. The caller already knows what it added, and a list
function would hand back handles whose ownership is ambiguous — every other handle here has
exactly one owner and one release.

`test/Sender.c` covers it against a live connection: replacing with a same-kind track, with null,
and back; a video track refused on an audio sender; the connection still connected and *no*
renegotiation raised by any of it; removal, second removal, the null-argument paths, and release.

## Desktop capture — **implemented**

`getDisplayMedia`, taken apart the same way `getUserMedia` was: enumerate, then create a track
from a chosen source.

```c
rtc_desktop_source_count(kind, &count);              /* screen or window     */
rtc_desktop_source_info(kind, index, &title, &id);   /* title caller-frees   */
rtc_desktop_track_create(factory, kind, id, label, max_fps, &track);
```

WebRTC ships `modules/desktop_capture` but nothing that turns a `DesktopCapturer` into a video
track, so the shim supplies that bridge — the same gap `CameraSource` fills for the camera.

**The source is identified by id, not index.** The list is rebuilt on every call and windows come
and go, so an index taken from one call means nothing in the next.

**A `DesktopCapturer` must be created, used and destroyed on one thread**, and that thread cannot
be the caller's, because capture has to keep running after `rtc_desktop_track_create` returns. So
the source owns a capture thread and does all three there. Creation waits for the capturer to come
up, which is what lets a bad source id fail the create call rather than yield a track that never
produces a frame.

**The loop is paced to a deadline, not by sleeping after each capture.** Capturing a 4K screen
through GDI costs tens of milliseconds; adding that to the interval halved the delivered rate.
Measured on a 3840x2160 screen: 8.5 fps against a requested 15 before the fix, 15.4 after.

**`is_screencast()` returns true**, which shifts the encoder's degradation preference towards
holding resolution rather than frame rate. Screen content is read, not watched — text staying
legible when the link tightens is the whole difference between a useful shared screen and a
useless one.

Windows uses the GDI capturers. DirectX and Windows Graphics Capture are faster but need COM or
WinRT initialised on the capture thread, which is a larger contract than this owes its caller.

`test/DesktopCapture.c` enumerates both kinds, captures a screen, and checks the frames — the
conversion from BGRA to I420 is ours rather than a capture module's, so a stride or plane mistake
would give a sheared or grey image that every status code would still call success.

## Still out of scope

`getStats`, transceivers and `getUserMedia` constraint negotiation, simulcast, insertable
streams, DTMF, ICE restart, receivers, audio device *selection*
(enumeration works; `audio_track_create` takes no device id), and an `on_ice_gathering_state`
callback — without which a caller can observe gathering starting but never completing. Each is
additive and none changes the conventions above.

## Working notes

- **Design against `WebRTCme.Api` first.** Its shape is fixed and mirrors W3C; the ABI exists to
  serve it, not the other way round.
- **[webrtc-sdk/libwebrtc](Prebuilt-distributions) is a useful blueprint but not a dependency.** It
  is MIT and its header set maps cleanly onto W3C, so it is worth reading for API shape. It is not
  adopted because it builds against webrtc-sdk's *fork*, pinned to `m150_release` with a 363-line
  core-audio patch per milestone — which would break the branch auto-detection this repository is
  built around.
- **The shim is platform-neutral C++.** Linux needs a new workflow, not new interop code, and
  `Bindings.Maui.Windows` becomes the template for the Linux binding.
- **Build before believing the page.** Three things in this contract were wrong until the first
  DLL was produced: the exception guard above, the assumption that `rtc::` names still exist (M152
  moved everything into `namespace webrtc`), and the omission of `WEBRTC_LIBRARY_IMPL` from the GN
  defines — without which `RTC_EXPORT` becomes `dllimport`, and lld-link rejects every call to
  WebRTC with `LNK4217`.
- **Verify the ownership rule with a leak test before growing the surface.** Create and release a
  thousand peer connections and watch the process working set. Getting this wrong is cheap to fix
  at twenty functions and expensive at two hundred.

## See also

- [Repository layout](Repository-layout) — where `WebRtcInterop/` sits and what it contains today
- [Workflow reference](Workflow-reference) — how the shim is grafted into the WebRTC build
- [Platform layers](Platform-layers) — why Windows has no SDK to bind to
