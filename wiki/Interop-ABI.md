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
| `rtc_peer_connection_observer` | 40 | five function pointers |

The header also compiles clean as C11 and as C++17 under `/W4`, so it can be consumed by a C
caller, a C++ caller, or read as documentation without a toolchain.

## Slice one

The smallest surface that carries an audio and video call between two Windows peers. Twenty-five
functions, declared in `WebRtcInterop/include/Interop.h`. Everything else waits until this works
end to end.

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

### Devices

```c
rtc_status rtc_video_device_count(rtc_factory* f, int32_t* out_count);
rtc_status rtc_video_device_info(rtc_factory* f, int32_t index,
                                 char** out_name, char** out_id);
rtc_status rtc_audio_device_count(rtc_factory* f, int32_t* out_count);
rtc_status rtc_audio_device_info(rtc_factory* f, int32_t index,
                                 char** out_name, char** out_id);
```

Backed by `modules/video_capture` (DirectShow) and `modules/audio_device` (WASAPI), both of which
are present in `webrtc.dll` on Windows. Both output strings are caller-owned.

### Tracks

```c
rtc_status rtc_audio_track_create(rtc_factory* f, const char* label,
                                  rtc_media_track** out_track);
rtc_status rtc_video_track_create(rtc_factory* f, const char* device_id,
                                  int32_t width, int32_t height, int32_t fps,
                                  rtc_media_track** out_track);
rtc_status rtc_media_track_set_enabled(rtc_media_track* t, int32_t enabled);
rtc_status rtc_media_track_get_id(rtc_media_track* t, char** out_id);
void       rtc_media_track_release(rtc_media_track* t);
```

This is `getUserMedia` reduced to its parts: enumerate, then create a track from a chosen device.
The constraint negotiation `WebRTCme.Api` exposes is resolved on the C# side, which then asks for
concrete numbers.

### Peer connection

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

### Negotiation

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

### Video frames

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

**The riskiest part of slice one.** Frames arrive on a WebRTC thread at 30 fps and the buffers are
valid only for the duration of the callback, so the handler must copy or convert immediately.
Anything slower than the frame interval will drop frames or stall decoding.

Included because without it there is nothing to look at, and "a working call" cannot be
demonstrated. Expect this to be the piece that needs revisiting for performance.

## Deliberately out of slice one

Data channels, `getStats`, transceivers and `getUserMedia` constraint negotiation, simulcast,
screen capture via `rtc_desktop_capturer`, insertable streams, DTMF, and renegotiation beyond a
single offer/answer. Each is additive and none changes the conventions above.

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
