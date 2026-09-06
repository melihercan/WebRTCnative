# Consuming the artifacts

The binaries built here feed [WebRTCme](https://github.com/melihercan/WebRTCme). This page maps
each artifact to the project that consumes it.

## Where each artifact goes

| Artifact | WebRTCme project | Role |
|---|---|---|
| `webrtc.dll` | `WebRTCme.Bindings/WebRTCme.Bindings.Native` | Windows P/Invoke target |
| `libwebrtc.so` | `WebRTCme.Bindings/WebRTCme.Bindings.Native` | Linux P/Invoke target |
| `libwebrtc.dylib` | `WebRTCme.Bindings/WebRTCme.Bindings.Native` | macOS P/Invoke target |
| `libwebrtc.aar` | `WebRTCme.Bindings/Maui/WebRTCme.Bindings.Maui.Android` | Java binding source |
| `WebRTC.xcframework` | `WebRTCme.Bindings/Maui/WebRTCme.Bindings.Maui.iOS` | ObjC binding source |
| `webrtc.lib`, `libwebrtc.a` | *(none)* | for linking native C/C++ against WebRTC |

[Platform layers](Platform-layers) records what each of these artifacts actually contains — the
desktop ones are missing H.264 entirely, and the macOS one has no camera capture. Read it before
assuming a capability is present.

The static libraries are not used by WebRTCme. They exist for anyone building a native component
that links WebRTC directly, and because the static build is the sanity check that the source tree
compiles at all before the shared-library patch enters the picture.

## Desktop: P/Invoke

`WebRTCme.Bindings.Native` calls into the shared libraries. The .NET default probing rules apply,
so the file names matter: `webrtc.dll` on Windows, `libwebrtc.so` on Linux, `libwebrtc.dylib` on
macOS. The workflows already emit exactly those names.

Match the architecture to the app. `target_cpu=x64` for a `win-x64` app; `arm64` for Apple silicon.
A mismatch surfaces as `DllNotFoundException` or `BadImageFormatException` at the first call.

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
