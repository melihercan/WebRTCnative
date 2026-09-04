# Workflow reference

Eight workflows, all `workflow_dispatch` only — nothing runs on push or pull request. Building
WebRTC costs about an hour of runner time, so it happens when asked and not otherwise.

| Workflow | Runner | Output | Consumed by |
|---|---|---|---|
| `WebRtcNativeWindowsStaticLib` | `windows-latest` | `webrtc.lib` | native C/C++ linking |
| `WebRtcNativeWindowsDynamicLib` | `windows-latest` | `webrtc.dll` (+ import lib, PDB) | `WebRTCme.Bindings.Native` |
| `WebRtcNativeLinuxStaticLib` | `ubuntu-latest` | `libwebrtc.a` | native C/C++ linking |
| `WebRtcNativeLinuxSharedLib` | `ubuntu-latest` | `libwebrtc.so` | `WebRTCme.Bindings.Native` |
| `WebRtcNativeMacOsStaticLib` | `macos-latest` | `libwebrtc.a` | native C/C++ linking |
| `WebRtcNativeMacOsSharedLib` | `macos-latest` | `libwebrtc.dylib` | `WebRTCme.Bindings.Native` |
| `WebRtcNativeAndroidLib` | `ubuntu-latest` | `libwebrtc.aar` | `WebRTCme.Bindings.Maui.Android` |
| `WebRtcNativeIosLib` | `macos-latest` | `WebRTC.xcframework` | `WebRTCme.Bindings.Maui.iOS` |

## Shared skeleton

The six desktop workflows are the same job with three variables: operating system, whether the
[shared-library patch](Shared-library-patch) is applied, and which file is collected.

1. **Checkout** this repository (`actions/checkout@v4`) — needed for the composite action.
2. **Resolve WebRTC branch** — see [Branch selection](Branch-selection). Fails fast on a bad value.
3. **Free disk space** (Linux only) — remove preinstalled toolchains; a WebRTC checkout does not
   fit alongside them.
4. **Install depot_tools** and put it *first* on `PATH`, so its bundled Python and Git win over
   the runner's.
5. **Bootstrap gclient** — a bare `gclient` call, which downloads its own dependencies on first run.
6. **Configure Git** — `gclient` refuses to run without an identity. It is a bot identity, not a
   commit author for this repository.
7. **Fetch WebRTC source** — `fetch --nohooks`, then fetch the resolved branch-head explicitly and
   check it out, then `gclient sync -D --force --reset`.
8. **Patch for a shared library** — dynamic/shared workflows only.
9. **Build** — `gn gen` then `autoninja -C out/Default webrtc`.
10. **Collect** — locate the output by name and fail loudly if it is missing.
11. **Upload** (`actions/upload-artifact@v4`) with `if-no-files-found: error`.

### Why the branch-head is fetched explicitly

`git checkout branch-heads/NNNN` depends on the remote refspec already covering branch-heads. The
workflows do not rely on that:

```bash
git fetch origin "+refs/branch-heads/${BRANCH}:refs/remotes/branch-heads/${BRANCH}"
git checkout -B "webrtc-${BRANCH}" "refs/remotes/branch-heads/${BRANCH}"
```

### Why the build target is named

`autoninja -C out/Default` with no target builds the entire tree, including tests. The workflows
build `webrtc` specifically and pass `rtc_include_tests=false rtc_build_tools=false
rtc_build_examples=false`, none of which affect the contents of the library.

## Windows specifics

Everything lives on `C:`. The hosted runner's `D:` has roughly 14 GB free, nowhere near enough.

`DEPOT_TOOLS_WIN_TOOLCHAIN: 0` builds with the Visual Studio installed on the runner instead of
Google's internal toolchain, which is not publicly accessible. The install path is discovered with
`vswhere` and exported as `GYP_MSVS_OVERRIDE_PATH` / `vs2022_install`, rather than being hard-coded
to an edition that may change with the runner image.

`depot_tools` is downloaded as a zip and extracted with `7z`, not cloned — a plain clone on Windows
misses the bootstrap step.

The dynamic workflow also collects `webrtc.dll.lib` (the import library, needed to link from C/C++)
and `webrtc.dll.pdb` (symbols, which make crash dumps from P/Invoke callers readable).

## Linux specifics

The **Free disk space** step removes `/usr/share/dotnet`, `/usr/local/lib/android`, `/opt/ghc`,
`/usr/local/share/boost`, `/usr/local/.ghcup`, `/usr/share/swift` and `$AGENT_TOOLSDIRECTORY`, then
runs `apt-get clean`. Disk usage is printed before and after.

`build/install-build-deps` arrives with the gclient-pulled `build/` directory and has changed name
between a shell script and a Python script across branches, so the workflows probe for either.

## macOS specifics

`macos-latest` is Apple silicon, so `target_cpu` defaults to `arm64`; choose `x64` for Intel Macs.
The collect step runs `lipo -info` so the log records what was actually produced.

macOS ships BSD `sed`, where `-i` requires an explicit backup suffix. Every in-place edit is
`sed -i '' …`. Dropping the empty `''` makes `sed` treat the next argument as the suffix and the
edit silently goes to the wrong place — this is the single easiest way to break the macOS shared
build.

## Android specifics

Fetches `webrtc_android` rather than `webrtc`; that solution pulls the Android SDK and NDK that
`build_aar.py` needs.

No shared-library patch is involved — `build_aar.py` already produces an `.aar` containing JNI
shared objects plus the Java API.

The `arch` input is space-separated ABI names. Leaving it empty builds `build_aar.py`'s own
defaults: `armeabi-v7a`, `arm64-v8a`, `x86`, `x86_64`. The collect step lists the `.so` files
inside the archive so the log shows which ABIs really shipped.

This is the workflow most likely to run out of disk, which is why the space-clearing step matters
most here.

## iOS and Mac Catalyst specifics

Fetches `webrtc_ios`, then runs `tools_webrtc/ios/build_ios_libs.py`.

The default `arch` list is:

```
device:arm64 simulator:arm64 simulator:x64 catalyst:arm64 catalyst:x64
```

`build_ios_libs.py` accepts these values (`ENABLED_ARCHS`):

| Value | Slice |
|---|---|
| `device:arm64` | iOS device |
| `simulator:arm64` | iOS simulator on Apple silicon |
| `simulator:x64` | iOS simulator on Intel |
| `catalyst:arm64` | Mac Catalyst on Apple silicon |
| `catalyst:x64` | Mac Catalyst on Intel |
| `arm64`, `x64` | legacy aliases, device only |

Upstream's own default omits the catalyst slices. This repository includes them so a single
xcframework serves both `net10.0-ios` and `net10.0-maccatalyst` in WebRTCme.

> The previous version of this workflow passed `--arch arm64 x64`. Those legacy aliases mean
> *device only*, so the resulting xcframework had no simulator and no Catalyst slice.

The framework is zipped before upload. `upload-artifact` does not preserve symlinks, and an
xcframework that loses its symlinks will not link. The zip is created from inside `out_ios_libs`
so `WebRTC.xcframework` sits at the archive root, and uploaded with `compression-level: 0` because
it is already compressed.

## Action versions

| Action | Version |
|---|---|
| `actions/checkout` | v4 |
| `actions/upload-artifact` | v4 |
| `microsoft/setup-msbuild` | v2 |

`actions/upload-artifact@v3` was retired by GitHub; workflows still using it fail immediately.
Note that v4 artifacts are immutable and same-named uploads no longer merge — each workflow here
uploads exactly one artifact per run, so neither matters in practice.
