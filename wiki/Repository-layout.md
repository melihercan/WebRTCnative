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

| File | Contents |
|---|---|
| `include/Interop.h` | the public ABI — the only file a caller needs |
| `src/Internal.h` | handle definitions, shared between the translation units |
| `src/Interop.cc` | library lifecycle, factory, device enumeration, tracks |
| `src/PeerConnection.cc` | peer connection, observers, negotiation |
| `src/DataChannel.cc` | SCTP data channels |
| `src/FrameSink.cc` | video frame delivery |
| `test/` | C harnesses that load the built DLL and drive it |
| `BUILD.gn` | declares `rtc_shared_library("WebRtcInterop")` with its dependency list |
| `.clang-format`, `format.sh`, `NOTICE` | Chromium style, formatter, upstream notice |

### How it builds

`BUILD.gn` opens with `import("../webrtc.gni")` and depends on siblings such as `../api`, `../pc`
and `../media`, so it only resolves from **inside** a WebRTC checkout. The workflow therefore
copies this directory to `src/WebRtcInterop` and appends `"//WebRtcInterop"` to the root
`group("default")` deps so ninja reaches it. That graft is edit 6 on top of the standard
[shared-library patch](Shared-library-patch).

That is not merely convenient. `webrtc.dll` is compiled with clang against libc++, and anything
MSVC compiles has a different `std::string`, allocator and exception model — so the shim has to be
built by the same toolchain, from inside the same tree.

### The convention it follows

[Interop ABI](Interop-ABI) is the specification: the ownership rule, the threading rule, the string
rules, and the verified struct layouts. Read it before adding a function; the conventions are not
obvious from the header alone.

Everything the ABI page lists is implemented — the twenty-five functions of slice one, plus data
channels — and each area has a test in `test/` that drives it against the built DLL.

This work dates from 2023 and predates the current workflows. Its original standalone repository
was folded in here; the full history is preserved on the `archive/webrtcinterop-2023` branch.

## Conventions

- Default branch is `main`.
- C/C++ is Chromium style via `WebRtcInterop/.clang-format`; `format.sh` applies it and needs
  `dos2unix` and `clang-format` on `PATH`.
- There is no test suite, linter or package manifest. Validation of a workflow change is reading
  it, then dispatching it.
