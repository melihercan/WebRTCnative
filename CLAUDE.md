# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

CI-only repository that **builds the Google WebRTC native libraries** (from
[webrtc.googlesource.com](https://webrtc.googlesource.com/src/)) for desktop and mobile and
publishes them as GitHub Actions artifacts. There is no product code here — the deliverable is the
set of `.lib` / `.dll` / `.a` / `.so` / `.dylib` / `.aar` / `.xcframework` binaries the workflows
produce.

Those artifacts are the input to the sibling repo `C:\dev\WebRTCme`: desktop shared libraries feed
`WebRTCme.Bindings/WebRTCme.Bindings.Native`, and the `.aar` / `WebRTC.xcframework` feed
`WebRTCme.Bindings/Maui/*`.

Extensive documentation lives in `wiki/` (published to the GitHub wiki). **Read the relevant page
before changing a workflow** — most of the non-obvious decisions are recorded there rather than in
comments.

## Commands

There is no compiler, package manager or test runner in this repository — the build happens on
GitHub's runners. These are the commands that actually apply.

**Dispatch a build** (the only way to build; see "You cannot run these builds locally" below):

```powershell
gh workflow run WebRtcNativeLinuxStaticLib.yml
gh workflow run WebRtcNativeIosLib.yml -f webrtc_branch=7977
gh run watch
gh run download <run-id>
```

**Run the branch resolver** — the one component that is testable off CI:

```powershell
python .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py
python .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py --branch 7977
python -m py_compile .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py
```

> On this machine `python` is the MSYS2 build, which ships no CA bundle, so the resolver's HTTPS
> calls die with `CERTIFICATE_VERIFY_FAILED`. Point it at one first — the failure looks like a bug
> in the script and is not:
> ```powershell
> $env:SSL_CERT_FILE = 'C:\msys64\usr\ssl\certs\ca-bundle.crt'
> ```
> CI is unaffected; the runner images have proper certificates.

**Check a workflow parses** (nearest thing to a lint — `actionlint` is not installed):

```powershell
npx --yes js-yaml .github/workflows/WebRtcNativeAndroidLib.yml
```

**Format C/C++** (Chromium style; needs `dos2unix` and `clang-format` on `PATH`):

```powershell
bash WebRtcInterop/format.sh
```

**Publish the wiki** after editing `wiki/`:

```powershell
git clone https://github.com/melihercan/WebRTCnative.wiki.git
cp wiki/*.md WebRTCnative.wiki/
cd WebRTCnative.wiki; git add -A; git commit -m "Update wiki"; git push
```

## Layout

- `.github/workflows/` — ten `workflow_dispatch`-only build pipelines, one per platform and
  link type.
- `.github/actions/resolve-webrtc-branch/` — composite action plus `resolve_webrtc_branch.py`,
  the shared branch-resolution logic every workflow calls first.
- `wiki/` — the documentation source. Keep it in step with workflow changes.
- `WebRtcInterop/` — **the C ABI shim, in progress.** WebRTC's C++ API cannot be P/Invoked, so
  .NET needs a flat C surface; this is it, and it is the missing piece for a Windows binding that
  replaces SIPSorcery. Built by `WebRtcNativeInteropWindows`, the only workflow that compiles code
  from this repository. Its `BUILD.gn` only resolves from inside a WebRTC checkout, so the workflow
  grafts the directory to `src/WebRtcInterop` and adds `"//WebRtcInterop"` to the root group.
  `src/Interop.cc` has three factory exports and a stalled `CallCreatePeerConnectionFactory`;
  `include/Interop.h` and `test/Tests.cc` are still empty. Folded in from a standalone repo in
  2026; that history is on the `archive/webrtcinterop-2023` branch.
- `WebRtcNativeObjectsWrapper/` — **dormant.** VS CMake "Hello CMake" template plus a stubbed
  `extern "C" CreateXxxObject()` with a commented-out body. Nothing references it.
- `Links.md` — reading behind both dormant experiments.

## Branch selection — the core design decision

Nothing in this repository hard-codes a WebRTC version. Every workflow resolves one at run time.

WebRTC rides the Chromium release train: each milestone has a matching WebRTC branch and the
numbers are the same (Chromium M152 → `branch-heads/7977`).

**The rule: the highest milestone from `chromiumdash.appspot.com/fetch_milestones` whose
`schedule_phase` is exactly `"stable"`, then take its `webrtc_branch`.**

Two plausible alternatives are wrong, and were rejected deliberately — do not "simplify" to either:

- `fetch_releases?channel=Stable` returns the milestone *rolling out*, one ahead of broad stable
  (it answered M153 while M152 was stable).
- The largest `refs/branch-heads/*` in the WebRTC repo returns branches nobody ships (8043 while
  the stable branch was 7977).

Override, on every workflow: a single `webrtc_branch` input (e.g. `7977`); empty means
auto-detect. **One input on purpose** — milestone and branch are one to one, so a second box would
just be another way of naming the same thing, and it could not express a branch the dashboard no
longer lists. The branch is what the checkout uses, so nothing is translated between what is typed
and what is built; the milestone is still reported, via reverse lookup, as a label. The default is
**empty on purpose** — a `workflow_dispatch` default is a static string, so prefilling a number
would just relocate the hard-coded version into the input box.

The resolver validates the branch with `git ls-remote` before any build starts, and writes
`branch` / `milestone` / `source` outputs plus a job summary. Dashboard responses are guarded for
non-dict entries: an unknown milestone comes back as `[null]`, not `[]`.

It is deliberately a standalone script rather than inline shell, so it can be run and tested off
CI before you touch resolution logic (see Commands).

## The workflows

All ten are manual only — nothing runs on push or PR. Shared skeleton: checkout → resolve branch
→ (Linux: free disk) → install `depot_tools` first on `PATH` → bootstrap `gclient` → git identity →
`fetch --nohooks` + explicit branch-head fetch/checkout + `gclient sync` → (shared builds: patch) →
`gn gen` + `autoninja -C out/Default webrtc` → locate output → `upload-artifact@v4`.

| Workflow | Runner | Artifact |
|---|---|---|
| WindowsStaticLib | windows | `webrtc.lib` |
| WindowsDynamicLib | windows | `webrtc.dll` + `.dll.lib` + `.pdb` |
| LinuxStaticLib | ubuntu | `libwebrtc.a` |
| LinuxSharedLib | ubuntu | `libwebrtc.so` |
| MacOsStaticLib | macos | `libwebrtc.a` |
| MacOsSharedLib | macos | `libwebrtc.dylib` |
| AndroidLib | ubuntu | `libwebrtc.aar` |
| IosLib | macos | `WebRTC.xcframework.zip` (iOS slices) |
| MacCatalystLib | macos | `WebRTC.xcframework.zip` (Catalyst slices) |
| InteropWindows | windows | `WebRtcInterop.dll` + component DLLs |

### The shared-library patch

WebRTC only emits static libs. The three shared/dynamic workflows patch the checkout in place —
this is the highest-risk part of the repo, so **preserve all four edits and their assertions**:

1. `rtc_static_library` → `rtc_shared_library` in `BUILD.gn`
2. delete every `complete_static_lib` line from `BUILD.gn`
3. in `webrtc.gni`, replace `!build_with_chromium && is_component_build` with `false`
4. delete the `:frame_analyzer` dep from `rtc_tools/BUILD.gn`

then `gn gen` with `is_component_build=true rtc_enable_symbol_export=true`.

Each workflow **asserts every anchor exists before editing**. A find-and-replace that finds nothing
exits 0, so without the assertions a refactor upstream would silently yield a static library that
fails at run time in an application. If an assertion fires, update the edit — never delete the
assertion.

Same edits, three dialects: GNU `sed -i`, BSD `sed -i ''` (the empty suffix is **mandatory** on
macOS), PowerShell `(Get-Content …).replace(…)` / `-notmatch`.

### Platform quirks

- **Windows** — everything on `C:`; `D:` has ~14 GB free, nowhere near enough.
  `DEPOT_TOOLS_WIN_TOOLCHAIN: 0` uses the runner's VS instead of Google's internal toolchain, and
  the VS path is discovered with `vswhere` rather than hard-coded to an edition. `depot_tools` is
  unzipped with `7z`, not cloned.
- **Linux / Android** — a "Free disk space" step removes preinstalled toolchains; without it the
  checkout does not fit. `build/install-build-deps` arrives with the gclient-pulled `build/`
  directory and has changed between `.sh` and `.py`, so the workflows probe for either.
- **macOS** — `macos-latest` is Apple silicon, so `target_cpu` defaults to `arm64`.
- **Android** — fetches `webrtc_android` (pulls SDK/NDK). No shared-library patch; `build_aar.py`
  already produces JNI `.so`s plus the Java API.
- **iOS and Mac Catalyst** — two workflows, one toolchain: both fetch `webrtc_ios` and run
  `build_ios_libs.py`, differing only in `--arch` (`device:arm64 simulator:arm64 simulator:x64`
  versus `catalyst:arm64 catalyst:x64`). Split to match the bindings, which are separate projects.
  Upstream's default omits catalyst entirely, and the bare aliases `arm64` / `x64` mean *device
  only* — an old version used them and shipped an xcframework with neither simulator nor Catalyst.
  Both frameworks are zipped before upload because `upload-artifact` does not preserve the symlinks
  an xcframework needs. **Dispatch both against the same `webrtc_branch`** or a milestone rollover
  between runs leaves iOS and Catalyst on different WebRTC versions.

## What is actually in each artifact

The artifacts are not equivalent slices of the same library. WebRTC is a shared core, a native
media layer aimed at desktop, and an integration layer (`sdk/`) that exists only for Android, iOS
and macOS. Mobile builds the SDK target and gets hardware codecs, capture and rendering; desktop
builds the raw `webrtc` C++ target and gets none of those. `wiki/Platform-layers.md` has the full
matrix and the evidence.

Three consequences that will otherwise be rediscovered by debugging a null factory at run time:

- **Desktop has no H.264.** `rtc_use_h264` falls to `proprietary_codecs` outside a Chromium build,
  which is false, so OpenH264 is never compiled (verified: 0 objects). The DLL still exports 26
  H.264 symbols for SDP/RTP, but `h264.cc:174` is `return nullptr`. Browsers get H.264 from the
  same tree via `media_use_openh264` — one flag apart.
- **Mac Catalyst is built with the iOS toolchain but shipped separately.** It has its own
  workflow and its own binding project. Catalyst has the full integration layer — VideoToolbox
  H.264, AVFoundation capture, Metal rendering — but no screen capture, since
  `rtc_desktop_capture_supported` counts it as iOS rather than as a Mac.
- **The macOS dylib is built but unconsumed.** `modules/video_capture/` has `linux/` and
  `windows/` only, so it has no camera path — which costs nothing, because nothing uses it. Those
  workflows are kept deliberately as an Apple-silicon build check; do not "fix" them by switching
  to `sdk:mac_framework_objc` unless a native AppKit target appears.
- **Windows and Linux are the only platforms with no tier 3 at all**, and no upstream `sdk/`
  target to build. A Windows SDK for WebRTCme means writing or adopting a C ABI shim over
  `webrtc.dll`; see `wiki/Prebuilt-distributions.md`.
- **Screen capture is desktop-only.** `rtc_desktop_capture_supported` excludes mobile by
  definition.

## Working in this repo

- **You cannot run these builds locally.** A WebRTC checkout is ~30 GB and needs `depot_tools`.
  Dispatch from the Actions tab or `gh workflow run <file>.yml`. Roughly an hour per run.
- **The resolver is the one thing that is locally testable** — run it directly (see above) before
  changing resolution logic.
- Validate YAML edits by reading and parsing, not by executing. There is no test suite, linter or
  package manifest, and no `dotnet test` coverage despite the sibling repo being .NET. Node is
  available on this machine; `jq` and `pip` are not, which is why the resolver is stdlib-only
  Python and the parse check goes through `npx`.
- Actions are pinned to `actions/checkout@v4`, `actions/upload-artifact@v4`,
  `microsoft/setup-msbuild@v2`. v3 of upload-artifact was retired by GitHub and fails immediately —
  do not downgrade.
- `if-no-files-found: error` on every upload is deliberate: it stops a broken build from uploading
  an empty artifact and looking like a success.
- Builds name the `webrtc` target explicitly and disable tests/tools/examples. A bare
  `autoninja -C out/Default` builds the whole tree, which the runners do not have time or disk for.
- The `git config --global user.name/email` in each workflow only exists to keep `gclient` happy;
  it is a bot identity, not a commit author for this repo.
- **Update `wiki/` in the same change as the workflow it documents.**

## Notes

- Default branch is `main` (not `master`, unlike WebRTCme). Remote:
  `https://github.com/melihercan/WebRTCnative`.
- The GitHub wiki repo does not exist until a first page is created via the UI; `README.md`
  records the publish steps.
- MIT licensed; `WebRtcInterop/NOTICE` carries the upstream libwebrtc notice for vendored files.
