# Building locally

CI is how artifacts are produced. These scripts are for the other thing: iterating
on `WebRtcInterop` without waiting forty minutes to find out whether it compiles.

That distinction is the whole point. A full artifact build is a CI job and should
stay one — it needs a clean machine, the resolver, and the packaging steps. But a
shim change is a five-second rebuild once a checkout exists, and the difference
between a five-second loop and a forty-minute one decides how much gets verified
before anything is committed.

## What each one is for

| script | what it does | how long |
|---|---|---|
| `fetch-webrtc.ps1` | fetches the WebRTC source at a branch-head and syncs it | ~1 hour, once |
| `build-webrtc-dll.ps1` | applies the shared-library patch and builds `webrtc.dll` | ~40 min |
| `build-interop.ps1` | copies `WebRtcInterop/` into the checkout and builds the shim | seconds after the first |
| `build-interop-test.ps1` | compiles one `WebRtcInterop/test/*.c` harness against the built DLL | seconds |

Only `build-interop.ps1` and `build-interop-test.ps1` are part of a normal
day. The other two exist to get a checkout in the first place.

## Typical session

```powershell
# Once: about 30 GB of source, and an hour.
.\tools\local\fetch-webrtc.ps1 -Branch 8010

# Then, per change, in seconds:
.\tools\local\build-interop.ps1
.\tools\local\build-interop-test.ps1 -Name Handshake
cd C:\tmp\webrtc-build\webrtc-checkout\src\out\Default
.\Handshake.exe
```

Resolve the branch with the same logic CI uses rather than guessing:

```powershell
python .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py
```

`-Root` moves the checkout somewhere other than `C:\tmp\webrtc-build`; `-Repo`
points at a different `WebRtcInterop`. Both default sensibly.

## Two traps worth knowing

**`build-interop.ps1` stamps the files it copies.** `Copy-Item` preserves the
source's timestamp, and the repository's files are usually older than the last
build output, so ninja would decide there was nothing to do and silently keep the
previous DLL. That cost a confusing half hour once: a reverted source file,
rebuilt, still produced the old behaviour.

**The shim must be built by WebRTC's own toolchain**, which is why these graft it
into the checkout rather than building it standalone. `webrtc.dll` is clang with
libc++; the ABI exists precisely so no C++ type has to cross between that and
MSVC. `WebRtcInterop/BUILD.gn` only resolves from inside a checkout for the same
reason.

## Why CLAUDE.md says you cannot build locally

It says that about the *artifact* builds, and for those it is true — they need
depot_tools, a clean machine and roughly an hour, which is what CI is for. It is
not true of the shim iteration loop above, which is local, fast, and was how the
sender-parameter and frame-adaptation work was verified before any of it reached
CI. Including a controlled A/B: the same binary built with and without one call,
to find out which of two explanations was actually right.
