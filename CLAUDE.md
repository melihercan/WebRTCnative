# WebRTCnative

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

## Layout

- `.github/workflows/` — eight `workflow_dispatch`-only build pipelines, one per platform and
  link type.
- `.github/actions/resolve-webrtc-branch/` — composite action plus `resolve_webrtc_branch.py`,
  the shared branch-resolution logic every workflow calls first.
- `wiki/` — the documentation source. Keep it in step with workflow changes.
- `WebRtcInterop/` — **dormant.** A C-ABI shim meant to be dropped into a WebRTC checkout as a
  subdirectory (its `BUILD.gn` opens with `import("../webrtc.gni")` and depends on `../api`,
  `../pc`, `../media`). `include/Interop.h`, `src/Interop.cc` and `test/Tests.cc` are all **zero
  bytes** — only the build file and vendored files from
  [webrtc-sdk/libwebrtc](https://github.com/webrtc-sdk/libwebrtc) have content. No workflow builds it.
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

Overrides, on every workflow: `webrtc_branch` (e.g. `7977`) wins over `chromium_milestone`
(e.g. `152`); both empty means auto-detect. Defaults are **empty on purpose** — a
`workflow_dispatch` default is a static string, so prefilling a number would just relocate the
hard-coded version into the input box.

The resolver validates the branch with `git ls-remote` before any build starts, and writes
`branch` / `milestone` / `source` outputs plus a job summary. An unknown milestone comes back from
the dashboard as `[null]`, not `[]` — that case is handled explicitly.

It is a standalone script so it can be tested off CI:

```powershell
python .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py
python .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py --milestone 152
python .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py --branch 7977
```

## The workflows

All eight are manual only — nothing runs on push or PR. Shared skeleton: checkout → resolve branch
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
| IosLib | macos | `WebRTC.xcframework.zip` |

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
- **iOS** — fetches `webrtc_ios`. Default arch list is
  `device:arm64 simulator:arm64 simulator:x64 catalyst:arm64 catalyst:x64`, which is upstream's
  default **plus Catalyst**, so one xcframework covers `net10.0-ios` and `net10.0-maccatalyst`.
  The bare aliases `arm64` / `x64` mean *device only* — the old workflow used them and shipped an
  xcframework with no simulator or Catalyst slice. The framework is zipped before upload because
  `upload-artifact` does not preserve the symlinks an xcframework needs.

## Working in this repo

- **You cannot run these builds locally.** A WebRTC checkout is ~30 GB and needs `depot_tools`.
  Dispatch from the Actions tab or `gh workflow run <file>.yml`. Roughly an hour per run.
- **The resolver is the one thing that is locally testable** — run it directly (see above) before
  changing resolution logic.
- Validate YAML edits by reading and parsing, not by executing. There is no test suite, linter or
  package manifest. `npx --yes js-yaml <file>` is a quick parse check; Node is available on this
  machine, `jq` and `pip` are not.
- Actions are pinned to `actions/checkout@v4`, `actions/upload-artifact@v4`,
  `microsoft/setup-msbuild@v2`. v3 of upload-artifact was retired by GitHub and fails immediately —
  do not downgrade.
- `if-no-files-found: error` on every upload is deliberate: it stops a broken build from uploading
  an empty artifact and looking like a success.
- Builds name the `webrtc` target explicitly and disable tests/tools/examples. A bare
  `autoninja -C out/Default` builds the whole tree, which the runners do not have time or disk for.
- The `git config --global user.name/email` in each workflow only exists to keep `gclient` happy;
  it is a bot identity, not a commit author for this repo.
- Keep C/C++ formatted with `WebRtcInterop/.clang-format` (Chromium style) via
  `WebRtcInterop/format.sh`; it needs `dos2unix` and `clang-format` on `PATH`.
- **Update `wiki/` in the same change as the workflow it documents.**

## Notes

- Default branch is `main` (not `master`, unlike WebRTCme). Remote:
  `https://github.com/melihercan/WebRTCnative`.
- The GitHub wiki repo does not exist until a first page is created via the UI; `README.md`
  records the publish steps.
- MIT licensed; `WebRtcInterop/NOTICE` carries the upstream libwebrtc notice for vendored files.
