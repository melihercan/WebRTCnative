# Prebuilt distributions

This repository is not the only project solving this problem. Several others build the WebRTC
native libraries and publish the binaries, some covering platforms this one does not.

Worth knowing about for three reasons: to cross-check a build that is behaving oddly, to cover a
platform we do not build, and to decide honestly whether maintaining our own pipeline still earns
its keep.

Figures are a snapshot taken 2026-09-06 and will drift. "Last push" is the signal that matters —
a WebRTC builder that has not moved in a year is tracking a Chromium milestone nobody ships.

## Cross-platform builders

The closest peers to this repository.

| Project | ★ | Licence | Last push | Platforms |
|---|---|---|---|---|
| [crow-misia/libwebrtc-bin](https://github.com/crow-misia/libwebrtc-bin) | 226 | Apache-2.0 | 2026-08-21 | Android, iOS, Linux x64/arm/arm64, macOS x64/arm64, Windows x86/x64 |
| [shiguredo-webrtc-build/webrtc-build](https://github.com/shiguredo-webrtc-build/webrtc-build) | 289 | Apache-2.0 | 2026-09-04 | Android, iOS, macOS arm64, Windows x64, Ubuntu 20.04–26.04 x86_64/armv8, Raspberry Pi OS armv8, Jetson |
| [webrtc-sdk/webrtc](https://github.com/webrtc-sdk/webrtc) | 444 | BSD-3-Clause | 2026-09-04 | Fork with prebuilt releases; the upstream behind LiveKit and flutter-webrtc |

`crow-misia/libwebrtc-bin` is the most direct comparison. Its releases are versioned
`<milestone>.<branch>.x.y` — the current one is **152.7977.0.0**, the same branch this repository
resolves to. If a build here fails and you need to know whether the branch itself is at fault,
that is the fastest cross-check available.

`shiguredo-webrtc-build` has the widest platform coverage of anything listed here, including
Jetson and Raspberry Pi. Its documentation is largely in Japanese.

## Apple platforms

| Project | ★ | Licence | Last push | Notes |
|---|---|---|---|---|
| [stasel/WebRTC](https://github.com/stasel/WebRTC) | 628 | see repo | 2026-09-01 | iOS/macOS XCFramework via Swift Package Manager and CocoaPods; the best-known Apple distribution |
| [livekit/webrtc-xcframework](https://github.com/livekit/webrtc-xcframework) | 33 | MIT | 2026-09-05 | XCFramework built from the webrtc-sdk fork |
| [alexpiezo/WebRTC](https://github.com/alexpiezo/WebRTC) | 133 | see repo | **2021-11-09** | Widely linked, but unmaintained for years — listed so it is recognised as stale |

`stasel/WebRTC` is the reference point should a native AppKit target ever be needed — it
distributes the ObjC framework that our macOS `.dylib` lacks. WebRTCme does not need one today;
see [Platform layers](Platform-layers).

## C API wrappers

Not builders exactly — they wrap WebRTC's C++ API in a flat C ABI so it can be called by P/Invoke,
FFI or similar. Directly relevant to the dormant `WebRtcInterop/` in this repository.

| Project | ★ | Licence | Last push | Notes |
|---|---|---|---|---|
| [webrtc-sdk/libwebrtc](https://github.com/webrtc-sdk/libwebrtc) | 637 | MIT | 2026-09-03 | C++ wrapper for binary release, used by flutter-webrtc desktop |

This is the project `WebRtcInterop/` was started from — `WebRtcInterop/NOTICE` carries its MIT
notice, and `helper.h` and `BUILD.gn` came from it. Anyone reviving that directory should start by
comparing against the current upstream rather than the vendored 2023 snapshot.

## WebRTC inside larger products

These build WebRTC as part of something else. Useful mainly as worked examples of build
configuration.

| Project | ★ | Licence | Last push | Notes |
|---|---|---|---|---|
| [signalapp/ringrtc](https://github.com/signalapp/ringrtc) | 632 | AGPL-3.0 | 2026-09-04 | Signal's calling stack; carries its own WebRTC patches |
| [Unity-Technologies/com.unity.webrtc](https://github.com/Unity-Technologies/com.unity.webrtc) | 853 | see repo | 2026-08-18 | Unity package with native plugins per platform |
| [jitsi/webrtc](https://github.com/jitsi/webrtc) | 114 | see repo | 2026-04-16 | Mirror maintained for building react-native-webrtc |
| [flutter-webrtc/flutter-webrtc](https://github.com/flutter-webrtc/flutter-webrtc) | 4485 | MIT | 2026-09-04 | Consumes the webrtc-sdk builds rather than building its own |

## Not builders: independent implementations

These do not build Google's WebRTC at all — they reimplement the protocols. Listed because they
are frequently mistaken for alternatives to a build, and because WebRTCme already depends on one.

| Project | ★ | Language | Notes |
|---|---|---|---|
| [sipsorcery-org/sipsorcery](https://github.com/sipsorcery-org/sipsorcery) | 1937 | C# | **Used by `WebRTCme.Bindings.SipSorcery`** for desktop |
| [pion/webrtc](https://github.com/pion/webrtc) | 16759 | Go | The most widely used independent implementation |
| [webrtc-rs/webrtc](https://github.com/webrtc-rs/webrtc) | 5143 | Rust | Port of Pion |
| [algesten/str0m](https://github.com/algesten/str0m) | 620 | Rust | Sans-I/O design |

An independent implementation sidesteps the whole build problem — no `depot_tools`, no 30 GB
checkout, no patching. The trade-off is that it implements the protocols rather than shipping
Google's media engine, so the audio processing, bandwidth estimation and codec integration are
different code with different maturity.

## What using someone else's build would not change

Every gap described in [Platform layers](Platform-layers) is a property of *which GN target was
built*, not of who built it. A raw C++ `webrtc` target from any of these projects has the same
missing H.264, because that comes from `proprietary_codecs` defaulting false.

The exceptions are the projects that build the `sdk/` targets — the Apple distributions above, and
the Android outputs of the cross-platform builders. Those carry the integration layer, which is
what actually closes the gaps.

So the question "should we keep building our own?" is really two questions: whether we want control
over the branch and the shared-library patch (which these mostly do not provide — they ship static
libraries or platform frameworks), and whether we would rather consume the SDK targets than the raw
core. The second is the more interesting one.

## See also

- [Platform layers](Platform-layers) — what any given artifact actually contains
- [Branch selection](Branch-selection) — how this repository picks a WebRTC version
