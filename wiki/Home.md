# WebRTCnative

WebRTCnative builds the [Google WebRTC](https://webrtc.googlesource.com/src/) native libraries
for desktop and mobile, and publishes them as GitHub Actions artifacts.

There is no product code here. The repository is a build factory: its entire output is a set of
binaries that the [WebRTCme](https://github.com/melihercan/WebRTCme) framework consumes.

```
chromiumdash.appspot.com          webrtc.googlesource.com
   (which branch?)                     (the source)
          |                                  |
          +------------- GitHub Actions -----+
                              |
        +---------+-----------+-----------+---------+
        |         |           |           |         |
    webrtc.lib  libwebrtc.a  libwebrtc.so  .aar   WebRTC.xcframework
    webrtc.dll  libwebrtc.dylib
        |         |           |           |         |
        +---------+-----------+-----------+---------+
                              |
                          WebRTCme
             WebRTCme.Bindings.Native (desktop P/Invoke)
             WebRTCme.Bindings.Maui.Android / .iOS
```

## Start here

| Page | What it covers |
|---|---|
| [Running a build](Running-a-build) | Dispatching a workflow, the inputs, where the artifact lands |
| [Branch selection](Branch-selection) | How the WebRTC branch is chosen, and why it is no longer hard-coded |
| [Platform layers](Platform-layers) | What is actually inside each artifact, and what is missing |
| [Workflow reference](Workflow-reference) | All eight workflows, step by step |
| [Shared library patch](Shared-library-patch) | Turning WebRTC's static-only build into a DLL / .so / .dylib |
| [Consuming the artifacts](Consuming-the-artifacts) | Wiring the binaries into WebRTCme |
| [Troubleshooting](Troubleshooting) | Failures seen in practice and what they mean |
| [Repository layout](Repository-layout) | What each directory is, including the dormant ones |

## The short version

Every workflow is manual (`workflow_dispatch`). Run one from the **Actions** tab and leave the
inputs blank; it asks the Chromium dashboard which milestone is currently stable, builds that
milestone's WebRTC branch, and uploads the result. Expect roughly an hour.

To pin a specific version instead, fill in `webrtc_branch` (e.g. `7977`) — the WebRTC branch
number, which <https://chromiumdash.appspot.com/branches> lists next to each Chromium milestone.

## Why a separate repository

Building WebRTC needs `depot_tools`, a ~30 GB checkout and a long compile on a machine with the
right platform SDK. None of that belongs in an application repository, and no developer wants it
on their laptop. Keeping it here means WebRTCme consumes prebuilt binaries and the expensive part
happens on GitHub's runners, on demand.

## Version policy

WebRTC ships on the Chromium release train: each Chromium milestone has a matching WebRTC branch,
and the two carry the same number (Chromium M152 → `branch-heads/7977`).

These workflows track **the newest milestone that has actually reached stable**, resolved at run
time. Nothing in the repository names a version, so nothing goes stale on its own. See
[Branch selection](Branch-selection) for the mechanics.
