# WebRtcInterop

A flat C surface over Google's WebRTC, so .NET can reach it through P/Invoke. This is the Windows
equivalent of the Java SDK Android gets and the Objective-C SDK iOS gets — the layer Google does
not ship for desktop.

```
include/Interop.h      the public ABI — the only file a caller needs
src/Internal.h         handle definitions, shared between the translation units
src/Interop.cc         library lifecycle, factory, device enumeration, tracks
src/PeerConnection.cc  peer connection, observers, negotiation
src/FrameSink.cc       video frame delivery
test/                  C harnesses driving the built DLL
```

## This cannot be built on its own

`BUILD.gn` opens with `import("../webrtc.gni")` and depends on sibling targets such as `../api` and
`../pc`, so it only resolves from **inside** a WebRTC checkout. That is not a limitation to work
around: `webrtc.dll` is compiled with clang against libc++, and anything MSVC compiles has a
different `std::string`, allocator and exception model. The shim has to be built by the same
toolchain.

`WebRtcNativeInteropWindows` does that — it copies this directory to `src/WebRtcInterop` inside the
checkout and appends `"//WebRtcInterop"` to the root `group("default")` so ninja reaches it.

## Documentation

The ABI contract, the ownership and threading rules, and the reasoning behind them are the
[Interop ABI](https://github.com/melihercan/WebRTCnative/wiki/Interop-ABI) page of the wiki. Read it
before adding a function; the conventions are not obvious from the header alone.

## Tests

`test/` holds C harnesses that load the built DLL and drive it. They are Windows-only today —
`LoadLibrary`, `GetProcAddress`, `Sleep` — and would need a small platform shim for Linux.

| | |
|---|---|
| `Handshake.c` | a full offer/answer/ICE exchange between two peer connections |
| `FrameSink.c` | opens a camera and checks the delivered frames |
| `Devices.c` | enumerates every device kind and exercises the error paths |

Build one against the DLL produced by the workflow:

```
clang-cl /I include test/Handshake.c /Fe:out/Default/handshake.exe
```

Set `WEBRTC_INTEROP_VERBOSE` to route WebRTC's own logging to stderr. The ICE layer is silent
otherwise, and that switch is how a missing Winsock initialisation was found.
