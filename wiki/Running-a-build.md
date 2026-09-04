# Running a build

## From the web UI

1. Open the repository's **Actions** tab.
2. Pick a workflow from the left-hand list.
3. **Run workflow** → leave every input blank → **Run workflow**.

Blank inputs mean "build the latest stable Chromium milestone". The run starts by printing what it
resolved, and writes the same to the job summary:

> ### WebRTC source
> | | |
> |---|---|
> | Branch | `branch-heads/7977` |
> | Chromium milestone | M152 |
> | Resolved by | auto-detected (latest stable Chromium milestone) |

When it finishes, the artifact is at the bottom of the run page.

## From the command line

```bash
gh workflow run WebRtcNativeWindowsDynamicLib.yml
gh workflow run WebRtcNativeIosLib.yml -f chromium_milestone=152
gh workflow run WebRtcNativeAndroidLib.yml -f webrtc_branch=7977
gh run watch
gh run download <run-id>
```

## Inputs

Present on every workflow:

| Input | Default | Notes |
|---|---|---|
| `webrtc_branch` | *(empty)* | WebRTC branch-head, e.g. `7977`. Highest priority. |
| `chromium_milestone` | *(empty)* | Chromium milestone, e.g. `152`. Ignored if a branch is given. |

See [Branch selection](Branch-selection) for how these interact.

Platform-specific:

| Workflow | Input | Default | Options |
|---|---|---|---|
| Windows ×2 | `target_cpu` | `x64` | `x64`, `x86`, `arm64` |
| Linux ×2 | `target_cpu` | `x64` | `x64`, `arm64`, `arm` |
| macOS ×2 | `target_cpu` | `arm64` | `arm64`, `x64` |
| Android | `arch` | *(empty = all ABIs)* | space-separated, e.g. `arm64-v8a x86_64` |
| iOS | `arch` | `device:arm64 simulator:arm64 simulator:x64 catalyst:arm64 catalyst:x64` | see [Workflow reference](Workflow-reference) |
| iOS | `build_config` | `release` | `release`, `debug` |

macOS defaults to `arm64` because `macos-latest` runners are Apple silicon. Select `x64` for a
library that runs on Intel Macs.

## Artifacts

Names carry the milestone and branch, so downloads from different runs never collide:

| Workflow | Artifact | Contents |
|---|---|---|
| WindowsStaticLib | `webrtc-windows-x64-static-m152-7977` | `webrtc.lib` |
| WindowsDynamicLib | `webrtc-windows-x64-dynamic-m152-7977` | `webrtc.dll`, `webrtc.dll.lib`, `webrtc.dll.pdb` |
| LinuxStaticLib | `webrtc-linux-x64-static-m152-7977` | `libwebrtc.a` |
| LinuxSharedLib | `webrtc-linux-x64-shared-m152-7977` | `libwebrtc.so` |
| MacOsStaticLib | `webrtc-macos-arm64-static-m152-7977` | `libwebrtc.a` |
| MacOsSharedLib | `webrtc-macos-arm64-shared-m152-7977` | `libwebrtc.dylib` |
| AndroidLib | `webrtc-android-m152-7977` | `libwebrtc.aar` |
| IosLib | `webrtc-ios-m152-7977` | `WebRTC.xcframework.zip` |

Retention is 30 days. Download anything you intend to keep, or attach it to a release.

## How long it takes

Roughly an hour per workflow, dominated by `gclient sync` (a ~30 GB checkout) and the compile.
The workflows build the `webrtc` target specifically rather than the whole tree, and disable
tests, tools and examples, which cuts a substantial amount of both time and disk off the old
behaviour.

Runs are capped at `timeout-minutes: 360`.

## Running several at once

Each workflow is independent and they can run in parallel, subject to your account's concurrency
limits. macOS runners are the scarcest and the most expensive; queue those deliberately.

## Reproducing an earlier build

Read the branch number from the old run's job summary and pass it explicitly:

```bash
gh workflow run WebRtcNativeLinuxSharedLib.yml -f webrtc_branch=7922
```

This is exact for the WebRTC source. It is not a hermetic reproduction — the runner image,
Xcode/MSVC version and `depot_tools` all move independently.
