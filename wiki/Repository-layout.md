# Repository layout

```
.github/
  workflows/                       the nine build pipelines
  actions/resolve-webrtc-branch/   shared branch resolution
    action.yml
    resolve_webrtc_branch.py
wiki/                              source of these pages
WebRtcInterop/                     dormant, see below
WebRtcNativeObjectsWrapper/        dormant, see below
Links.md                           reference links
README.md
LICENSE                            MIT
```

## `.github/`

The working content of the repository. Nine workflows, described in
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

## `WebRtcInterop/` — dormant

Intended as a C-ABI shim over the WebRTC C++ API, giving P/Invoke a flat surface to call.

**The implementation was never written.** `include/Interop.h`, `src/Interop.cc` and
`test/Tests.cc` are all zero bytes. What exists is scaffolding:

| File | Status |
|---|---|
| `BUILD.gn` | complete, declares `rtc_shared_library("libwebrtc")` with real dependencies |
| `include/Interop.h` | **empty** |
| `src/Interop.cc` | **empty** |
| `test/Tests.cc` | **empty** |
| `helper.h` | vendored from webrtc-sdk/libwebrtc |
| `.clang-format` | Chromium style |
| `format.sh` | runs `dos2unix` + `clang-format` |
| `NOTICE` | upstream MIT notice for the vendored files |

No workflow builds this directory.

The `BUILD.gn` is not buildable from here in any case: it opens with `import("../webrtc.gni")` and
depends on siblings such as `../api`, `../pc` and `../media`. It is written to be dropped **into a
WebRTC checkout** as a subdirectory. Reviving it would mean writing the interop layer, copying the
directory into `src/`, and adding it to the build.

Note that WebRTCme's desktop support currently goes through SIPSorcery
(`WebRTCme.Bindings.SipSorcery`) as well as the native path, so this shim is not on the critical
path for anything today.

## `WebRtcNativeObjectsWrapper/` — dormant

A Visual Studio CMake template project — "Hello CMake" — plus a stub:

```cpp
#define EXPORT //// export directive for each platform
class Xxx;

extern "C" EXPORT Xxx * CreateXxxObject()
{
	////return new Xxx();
}
```

An experiment in exporting C++ objects to C#, left at the sketch stage. `CMakePresets.json` has
presets for Windows x64/x86, Linux and macOS. Nothing references it and no workflow builds it.

`Links.md` collects the reading behind both experiments: C++/C# interop, marshalling C++ classes,
and cross-platform CMake.

## Why keep the dormant directories

[webrtc-sdk/libwebrtc](https://github.com/webrtc-sdk/libwebrtc), the project `WebRtcInterop/` was
started from, is still actively maintained — see [Prebuilt distributions](Prebuilt-distributions).
Anyone reviving that directory should compare against it rather than the vendored snapshot.

They record an intended direction, and `WebRtcInterop/BUILD.gn` is a genuinely useful starting
point — the dependency list for a WebRTC shared library that exposes a custom API is not obvious.
They are documented here so that neither is mistaken for working code.

## Conventions

- Default branch is `main`.
- C/C++ is Chromium style via `WebRtcInterop/.clang-format`; `format.sh` applies it and needs
  `dos2unix` and `clang-format` on `PATH`.
- There is no test suite, linter or package manifest. Validation of a workflow change is reading
  it, then dispatching it.
