# Consuming the artifacts

The binaries built here feed [WebRTCme](https://github.com/melihercan/WebRTCme). This page maps
each artifact to the project that consumes it.

## Where each artifact goes

| Artifact | WebRTCme project | Role |
|---|---|---|
| `webrtc.dll` | *(none — see below)* | for linking native C/C++ against WebRTC |
| `WebRtcInterop.dll` + companions | `WebRTCme.Bindings/Maui/WebRTCme.Bindings.Maui.Windows` | the Windows P/Invoke target |
| `libwebrtc.so` | `WebRTCme.Bindings/WebRTCme.Bindings.Native` | Linux P/Invoke target |
| `libwebrtc.dylib` | *(none — see below)* | built for completeness, not consumed |
| `libwebrtc.aar` | `WebRTCme.Bindings/Maui/WebRTCme.Bindings.Maui.Android` | Java binding source |
| `WebRTC.xcframework` (iOS slices) | `WebRTCme.Bindings/Maui/WebRTCme.Bindings.Maui.iOS` | ObjC binding source |
| `WebRTC.xcframework` (Catalyst slices) | `WebRTCme.Bindings/Maui/WebRTCme.Bindings.Maui.MacCatalyst` | ObjC binding source |
| `webrtc.lib`, `libwebrtc.a` | *(none)* | for linking native C/C++ against WebRTC |

[Platform layers](Platform-layers) records what each of these artifacts actually contains — the
desktop ones are missing H.264 entirely. Read it before assuming a capability is present.

**iOS and Mac Catalyst are built separately**, matching the split on the bindings side.
`WebRtcNativeIosLib` produces device and simulator slices; `WebRtcNativeMacCatalystLib` produces
the Catalyst ones. Both are `WebRTC.xcframework`, so keep them in their own directories when you
download both — the artifact names disambiguate, the file names do not.

Pin `webrtc_branch` on both when refreshing Apple support, or a milestone rollover between the two
runs will leave iOS and Catalyst on different WebRTC versions.

`libwebrtc.dylib` for native macOS is built but has no consumer; those workflows are kept as a
check that the tree still builds on Apple silicon.

The static libraries are not used by WebRTCme. They exist for anyone building a native component
that links WebRTC directly, and because the static build is the sanity check that the source tree
compiles at all before the shared-library patch enters the picture.

## Windows: two DLLs, deliberately

Windows produces two artifacts that both contain WebRTC, and this is a choice rather than an
oversight.

`WebRtcInterop.dll` is the one WebRTCme uses — the [interop shim](Interop-ABI), and the Windows
equivalent of `libwebrtc.aar` or `WebRTC.xcframework`. It does **not** depend on `webrtc.dll`; it
absorbs the WebRTC code it needs, so the runtime set is:

```
WebRtcInterop.dll                                  ~17.7 MB
third_party_abseil-cpp_absl.dll
third_party_boringssl.dll
third_party_protobuf_protobuf_full_and_lite_library.dll
libc++.dll
```

All five must sit beside the application; the collect step gathers them together.

`webrtc.dll` from `WebRtcNativeWindowsDynamicLib` therefore has no consumer in WebRTCme. It is kept
because it is what someone linking WebRTC from native C or C++ would want, and because it is the
cheaper thing to build when only checking that the tree still compiles. The duplicated ~18 MB is
accepted knowingly.

Two alternatives were considered and rejected for now: making the shim link against `webrtc.dll`
so it shrinks to a thin wrapper, and retiring the dynamic workflow altogether. Either remains open.

## Desktop: P/Invoke

`WebRTCme.Bindings.Native` calls into the shared libraries. The .NET default probing rules apply,
so the file names matter: `libwebrtc.so` on Linux. The workflows already emit exactly those
names.

Match the architecture to the app. `target_cpu=x64` for a `win-x64` app. A mismatch surfaces as
`DllNotFoundException` or `BadImageFormatException` at the first call.

Note that WebRTC's C++ API cannot be P/Invoked directly — a C ABI shim is required between
`webrtc.dll` and any .NET caller. See [Prebuilt distributions](Prebuilt-distributions) for the
options.

`webrtc.dll.lib` from the Windows dynamic workflow is only needed to link from C/C++. P/Invoke does
not use it. `webrtc.dll.pdb` is worth keeping alongside the DLL — it makes a native crash inside a
P/Invoke call readable instead of a bare address.

## Android

`libwebrtc.aar` is the input to the Java binding project. Confirm the ABIs before wiring it in:

```bash
unzip -l libwebrtc.aar | grep '\.so$'
```

The default build covers `armeabi-v7a`, `arm64-v8a`, `x86` and `x86_64` — device and emulator both.

## iOS and Mac Catalyst

Unzip `WebRTC.xcframework.zip` and check what came out:

```bash
unzip -q WebRTC.xcframework.zip
ls -1 WebRTC.xcframework
```

With this repository's default arch list you should see slices for iOS device, both simulators,
and Mac Catalyst on both architectures. One xcframework therefore covers `net10.0-ios` and
`net10.0-maccatalyst`.

**Preserve the symlinks.** Unzip with a tool that keeps them (`unzip` and Finder both do). A
framework whose symlinks were flattened will fail to link, often with a confusing error about a
missing binary. This is why the workflow uploads a zip rather than the directory.

### Regenerating the ObjC bindings

When the WebRTC version moves, the ObjC API may have changed and the C# bindings need regenerating
with [Objective Sharpie](https://aka.ms/objective-sharpie). `WebRTCme.Bindings/README_BuildBindings.txt`
in the WebRTCme repository records the procedure, including the header rewrite from `<WebRTC/x.h>`
to `"x.h"` that Sharpie needs.

## Recording what you shipped

Nothing in either repository pins a WebRTC version any more, which is the point — but that makes it
your responsibility to record what a given WebRTCme build was made against. The branch number is in
the run's job summary and in the artifact name (`…-m152-7977`).

Keeping the artifact name intact when you copy binaries into WebRTCme, or noting the branch in
`WebRTCme.Bindings/README_NativeSdkVersions.txt`, is enough to answer "which WebRTC is this?"
later.

## Upgrading to a newer WebRTC

1. Run the workflow for each platform you ship, inputs blank.
2. Check each run's summary — they should all report the same branch. (If a milestone rolls over
   mid-sequence they could differ; pin `webrtc_branch` explicitly to avoid a mixed set.)
3. Download the artifacts and drop them into the binding projects.
4. Regenerate the Android and iOS bindings if the native API moved.
5. Build and exercise the demo apps on each platform before trusting the result.

Step 5 is not optional. A WebRTC upgrade can change ObjC/Java signatures without any build error on
the .NET side until run time.
