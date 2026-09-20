# Troubleshooting

## Branch resolution

**`No milestone is in the 'stable' phase on the Chromium dashboard right now.`**

The dashboard is briefly inconsistent, usually during a milestone rollover. Pass an explicit
`webrtc_branch` and carry on; check <https://chromiumdash.appspot.com/branches> to pick one.

**`refs/branch-heads/NNNN does not exist in the WebRTC repository.`**

The branch number is wrong. The commonest cause is passing a Chromium milestone (`152`) where the
WebRTC branch (`7977`) is wanted; <https://chromiumdash.appspot.com/branches> maps one to the
other.

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

**`gn gen` fails in `setup_toolchain.py`: `Path "...\Windows Kits\10\include\10.0.NNNNN.0\um"
from environment variable "include" does not exist.`**

Chromium pins an exact Windows SDK version and hands it to `vcvarsall`, and the runner does not
have that version. M153 moved the pin to 10.0.28000 while `windows-latest` shipped 10.0.26100.

The workflow installs the pinned SDK before the fetch, and a later step reads the pin out of
`build/vs_toolchain.py` and `build/toolchain/win/setup_toolchain.py` and fails naming the version
it wants. If this appears again, a milestone has moved the pin: update the download link in
**Install the Windows SDK Chromium pins**.

**Do not point the checkout at an older SDK instead.** It is the obvious fix and it does not work:
`gn gen` passes, then libvpx fails to compile with `unknown type name 'FILE_INFO_BY_HANDLE_CLASS'`,
25 minutes later. The pin is a real dependency, not a version-string preference.

**`LLVM ERROR: IO failure on output stream: no space on device`, during the final link.**

Disk, reported as a linker crash that says nothing about disk. The build step prints free space
either side of itself for exactly this reason. Two things keep it inside the runner:
`symbol_level = 1` — `is_debug = false` does *not* reduce debug info, and a component build
otherwise writes a PDB beside every one of ~4700 targets, none of which anything consumes — and
the cleanup step that strips the image before the checkout lands.

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

**The AAR builds and uploads, and the consuming app fails with `class file has wrong version 69.0,
should be 65.0`.**

Nothing is wrong with the archive. It is compiled at a newer Java release than the toolchain
reading it supports — M153's Chromium builds at `--release 25`, which emits class file major 69,
and .NET Android's javac stops at 65.

`JAVA_RELEASE` in the workflow sets the level for all three producers: Chromium's
`compile_java.py` and `turbine.py`, patched in the checkout, and `tools/inject_gen_jni.py`. Keep
them in step. The injector is easy to forget and its omission is subtle — with no `--release` it
emits the bundled JDK's default, which put exactly one major-69 class into an otherwise major-65
archive, and that went unnoticed until something tried to read that class.

The collect step prints the class file majors it finds and fails above the ceiling
(`JAVA_RELEASE + 44`), so this should now be caught where the archive is built rather than in an
app that will not start. Raise `JAVA_RELEASE` only when the consumer can read the result.

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
