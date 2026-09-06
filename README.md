# WebRTCnative

Automated builds of the [WebRTC](https://webrtc.googlesource.com/src/) native libraries for
desktop and mobile, published as GitHub Actions artifacts and consumed by
[WebRTCme](https://github.com/melihercan/WebRTCme).

Full documentation is in the [wiki](../../wiki).

## Running a build

Open the **Actions** tab, pick a workflow, click **Run workflow**, and leave the inputs blank.

Blank means *build the latest stable Chromium milestone*: the workflow asks
[chromiumdash](https://chromiumdash.appspot.com/branches) which milestone is currently stable and
builds that milestone's WebRTC branch. No version is hard-coded anywhere, so nothing goes stale.

To pin a version, set `webrtc_branch` to a WebRTC branch-head number, e.g. `7977`. That is the
branch number, not the Chromium milestone (`152`) —
[chromiumdash](https://chromiumdash.appspot.com/branches) lists the two side by side.

Every run prints the branch it resolved and records it in the job summary. Expect about an hour.

## What gets built

| Workflow | Runner | Artifact |
|---|---|---|
| `WebRtcNativeWindowsStaticLib` | windows | `webrtc.lib` |
| `WebRtcNativeWindowsDynamicLib` | windows | `webrtc.dll` + import lib + PDB |
| `WebRtcNativeLinuxStaticLib` | ubuntu | `libwebrtc.a` |
| `WebRtcNativeLinuxSharedLib` | ubuntu | `libwebrtc.so` |
| `WebRtcNativeMacOsStaticLib` | macos | `libwebrtc.a` |
| `WebRtcNativeMacOsSharedLib` | macos | `libwebrtc.dylib` |
| `WebRtcNativeAndroidLib` | ubuntu | `libwebrtc.aar` |
| `WebRtcNativeIosLib` | macos | `WebRTC.xcframework` (iOS slices) |
| `WebRtcNativeMacCatalystLib` | macos | `WebRTC.xcframework` (Catalyst slices) |
| `WebRtcNativeInteropWindows` | windows | `WebRtcInterop.dll` |

Artifact names carry the milestone and branch, e.g. `webrtc-linux-x64-shared-m152-7977`.

### Desktop

Static libraries are what WebRTC's build system produces by default. Shared libraries are often
what you actually need — interoperating with C# through P/Invoke, for one — so the shared and
dynamic workflows patch the checkout before generating build files. See
[Shared library patch](../../wiki/Shared-library-patch) for each edit and why it is there.

### Mobile

Android, iOS and Mac Catalyst libraries are built for the .NET MAUI bindings. Catalyst has its own
workflow rather than sharing the iOS xcframework, matching the split on the bindings side; both use
the same iOS toolchain and differ only in the arch list.

## Documentation

| Page | |
|---|---|
| [Running a build](../../wiki/Running-a-build) | inputs, artifacts, timings |
| [Branch selection](../../wiki/Branch-selection) | how the WebRTC branch is chosen |
| [Workflow reference](../../wiki/Workflow-reference) | all ten workflows in detail |
| [Shared library patch](../../wiki/Shared-library-patch) | the static-to-shared conversion |
| [Consuming the artifacts](../../wiki/Consuming-the-artifacts) | wiring the output into WebRTCme |
| [Troubleshooting](../../wiki/Troubleshooting) | failures and what they mean |
| [Repository layout](../../wiki/Repository-layout) | what each directory is |

The pages are written in [`wiki/`](wiki) and published to the GitHub wiki:

```bash
git clone https://github.com/melihercan/WebRTCnative.wiki.git
cp wiki/*.md WebRTCnative.wiki/
cd WebRTCnative.wiki && git add -A && git commit -m "Update wiki" && git push
```

The wiki repository only exists once a first page has been created through the GitHub UI.

## Licence

MIT. `WebRtcInterop/NOTICE` carries the upstream notice for files vendored from
[webrtc-sdk/libwebrtc](https://github.com/webrtc-sdk/libwebrtc).
