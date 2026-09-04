# Troubleshooting

## Branch resolution

**`No milestone is in the 'stable' phase on the Chromium dashboard right now.`**

The dashboard is briefly inconsistent, usually during a milestone rollover. Pass an explicit
`chromium_milestone` or `webrtc_branch` and carry on; check
<https://chromiumdash.appspot.com/branches> to pick one.

**`refs/branch-heads/NNNN does not exist in the WebRTC repository.`**

The branch number is wrong. Milestone numbers (152) and branch numbers (7977) are easy to
transpose — `chromium_milestone` takes the former, `webrtc_branch` the latter.

**`The Chromium dashboard has no WebRTC branch for milestone N.`**

That milestone has not branched yet, or is old enough to have been dropped. The dashboard returns
the JSON body `[null]` for these.

**Milestone shows as `unknown` in the summary.**

Only happens with an explicit `webrtc_branch` that the dashboard does not track. Cosmetic — the
build proceeds.

## Disk space

**`No space left on device`, usually during `gclient sync` or deep into linking.**

The most common failure, and Android is the most affected. Check the `df -h /` output the
**Free disk space** step prints before and after.

A WebRTC checkout is around 30 GB before anything is compiled. If a runner image starts shipping
more preinstalled software, extend the removal list in the workflow. On Windows the equivalent
mistake is letting anything land on `D:`, which has roughly 14 GB free — everything is pinned to
`C:` for that reason.

## Shared library patch

**`Expected to find 'X' in Y. The shared-library patch needs updating for this WebRTC branch.`**

Working as intended: an anchor the patch depends on is gone from the new branch. Upstream
refactored. Read [Shared library patch](Shared-library-patch), work out the current equivalent,
update the edit *and* its assertion. Do not delete the assertion to get past it — you would get a
static library that fails later, in an application.

**The build succeeds but the library exports nothing.**

Symptoms: `DllNotFoundException` from .NET, or an empty `dumpbin /exports` / `nm -D` listing.
Edit 3 (`webrtc.gni`) or `rtc_enable_symbol_export=true` did not take. On macOS, suspect a
`sed -i` missing its `''` first.

## Windows

**`vswhere could not find a Visual Studio installation.`**

The runner image changed. Check what `windows-latest` currently maps to; the step deliberately
discovers the path rather than hard-coding an edition, so this means VS itself is missing or moved.

**`gclient` or `gn` not found.**

The `depot_tools` directory must be first on `PATH`. If the download or `7z` extraction failed
earlier the step usually reports it — read the Install depot_tools step, not the failing one.

**Toolchain errors mentioning Google's internal toolchain.**

`DEPOT_TOOLS_WIN_TOOLCHAIN: 0` is missing or was overridden. Without it depot_tools tries to fetch
a toolchain only Google can access.

## macOS

**Edits appear to apply but the build is still static.**

BSD `sed -i` without the empty backup suffix. Every macOS edit must be `sed -i '' …`.

**Architecture mismatch at run time.**

`macos-latest` is Apple silicon and the workflows default to `arm64`. For Intel Macs pass
`target_cpu: x64`. `lipo -info`, printed by the collect step, records what was built.

## iOS

**The xcframework has no simulator or Catalyst slice.**

The `arch` input was narrowed, or the legacy aliases `arm64` / `x64` were used — those mean *device
only*. The default list is
`device:arm64 simulator:arm64 simulator:x64 catalyst:arm64 catalyst:x64`.

**The framework will not link after unzipping.**

Symlinks were flattened by the extraction tool. Use `unzip` or Finder. This is also why the
artifact is a zip rather than a directory: `upload-artifact` does not preserve symlinks.

**Xcode or SDK errors after a runner image update.**

The Show toolchain step records `xcodebuild -version` and the SDK version for exactly this
comparison. A WebRTC branch may predate the Xcode on the runner; try a newer branch, or an older
runner label.

## Android

**`build_aar.py` fails early.**

Almost always disk (see above) or an incomplete `gclient sync`. The workflow must fetch
`webrtc_android`, not `webrtc` — that solution is what pulls the SDK and NDK.

**Missing ABIs in the AAR.**

The collect step lists the packaged `.so` files. If one is absent, the `arch` input restricted it;
leave `arch` empty for the full default set.

## Artifacts

**`if-no-files-found: error` fired.**

The build produced nothing under the expected name. The real failure is earlier — read the Build
step. This check exists so a broken build cannot upload an empty artifact and look like a success.

**Artifact already exists.**

`upload-artifact@v4` artifacts are immutable and same-named uploads no longer merge. Each workflow
uploads once per run and names include the branch, so this should not occur; if it does, two runs
are colliding.

## General

**A run takes far longer than an hour.**

Usually `gclient sync` fighting the network, or a cold runner. The 360-minute cap will stop it.
Re-running is normally cheaper than investigating.

**Reproducing a past build.**

Pass the branch from the old run's summary. Exact for the WebRTC source, but not hermetic — the
runner image, compiler and `depot_tools` all move on their own.
