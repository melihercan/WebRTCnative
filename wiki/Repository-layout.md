# Repository layout

```
.github/
  workflows/                       the ten build pipelines
  actions/resolve-webrtc-branch/   shared branch resolution
    action.yml
    resolve_webrtc_branch.py
WebRtcInterop/                     the C ABI shim — see below
  include/Interop.h                the public ABI
  src/                             implementation
  test/                            C harnesses driving the built DLL
tools/make_platform_diagram.py     regenerates the wiki figure
wiki/                              source of these pages
README.md
LICENSE                            MIT
```

Everything here is live. Two dormant experiments — `WebRtcNativeObjectsWrapper/`, a CMake
"Hello CMake" sketch, and `Links.md`, a bookmark dump behind it — were removed once
`WebRtcInterop/` became real; they are in the history if ever wanted.

## `.github/`

The working content of the repository. Ten workflows, described in
[Workflow reference](Workflow-reference), plus one composite action holding the branch-resolution
logic they all share ([Branch selection](Branch-selection)).

The resolver is a real Python script rather than inline shell so that it can be run and tested
outside CI:

```bash
python .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py --branch 7977
```

## `wiki/`

These pages, kept in the repository so they are reviewed alongside the workflows they describe.
The GitHub wiki is published from them; the repository `README.md` records the copy step.

## `WebRtcInterop/` — the C ABI shim

WebRTC's API is C++ and cannot be P/Invoked directly, so .NET needs a flat C surface in front of
it. That is what this directory is, and it is the missing piece for a Windows binding that replaces
SIPSorcery.

Built by [WebRtcNativeInteropWindows](Workflow-reference), which is the only workflow here that
compiles code from this repository rather than from Google's tree.

| File | Status |
|---|---|
| `BUILD.gn` | declares `rtc_shared_library("WebRtcInterop")` with its dependency list |
| `src/Interop.cc` | three factory exports, plus a stalled `CallCreatePeerConnectionFactory` |
| `include/Interop.h` | **empty** — the exports have no declared header yet |
| `test/Tests.cc` | **empty** |
| `helper.h` | vendored from webrtc-sdk/libwebrtc |
| `.clang-format`, `format.sh`, `NOTICE` | Chromium style, formatter, upstream notice |

### How it builds

`BUILD.gn` opens with `import("../webrtc.gni")` and depends on siblings such as `../api`, `../pc`
and `../media`, so it only resolves from **inside** a WebRTC checkout. The workflow therefore
copies this directory to `src/WebRtcInterop` and appends `"//WebRtcInterop"` to the root
`group("default")` deps so ninja reaches it. That graft is edit 6 on top of the standard
[shared-library patch](Shared-library-patch).

### Where the work stopped

`Interop.cc` exports `CreateBuiltinAudioEncoderFactory`, `CreateBuiltinVideoEncoderFactory` and
`CreateBuiltinVideoDecoderFactory`. The video ones use `.release()` on a `unique_ptr`, which is
sound; the audio one takes the address of a temporary `scoped_refptr`, which is not — it returns a
dangling pointer.

`CallCreatePeerConnectionFactory` is commented out entirely. That is the real problem this shim has
to solve: WebRTC hands back `scoped_refptr` and `unique_ptr`, and neither survives a C boundary
without an explicit ownership convention.

[Interop ABI](Interop-ABI) settles that convention and specifies the first slice. The 2023 code
predates it and does not follow it — treat the directory as a build harness that works, and the ABI
page as what to write into it.

This work dates from 2023 and predates the current workflows. Its original standalone repository
was folded in here; the full history is preserved on the `archive/webrtcinterop-2023` branch.

## Conventions

- Default branch is `main`.
- C/C++ is Chromium style via `WebRtcInterop/.clang-format`; `format.sh` applies it and needs
  `dos2unix` and `clang-format` on `PATH`.
- There is no test suite, linter or package manifest. Validation of a workflow change is reading
  it, then dispatching it.
