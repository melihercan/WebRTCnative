# Shared library patch

WebRTC's build system produces **static** libraries only. That is fine for a C++ application, and
useless for P/Invoke from C#, which needs a `.dll`, `.so` or `.dylib` exporting symbols.

Three workflows — `WindowsDynamicLib`, `LinuxSharedLib`, `MacOsSharedLib` — patch the checkout
in place before generating build files. This is the part of the repository with the least margin
for error, so it is documented in full.

## The four edits

All are applied to the WebRTC checkout at `src/`, after the branch is checked out and synced, and
before `gn gen`.

### 1. Emit shared libraries instead of static ones

```
BUILD.gn:  rtc_static_library  →  rtc_shared_library
```

`rtc_static_library` is WebRTC's own GN template. Swapping it for `rtc_shared_library` changes what
the top-level targets produce.

### 2. Drop `complete_static_lib`

```
BUILD.gn:  delete every line containing 'complete_static_lib'
```

`complete_static_lib` tells the archiver to fold all dependencies into one `.a`. It is meaningless
for a shared library, and GN rejects it on a `shared_library` target.

### 3. Stop `webrtc.gni` suppressing the component build

```
webrtc.gni:  !build_with_chromium && is_component_build  →  false
```

Line 16 of `webrtc.gni` guards a block that deliberately disables component builds when WebRTC is
built standalone. Since we *are* building standalone and *do* want a component build, the guard is
forced to `false`.

This is the edit that makes the difference between a genuine shared library and a shared-looking
one that still statically links everything.

### 4. Remove the `frame_analyzer` dependency

```
rtc_tools/BUILD.gn:  delete every line containing ':frame_analyzer'
```

`frame_analyzer` does not link under a component build. It is a developer tool, absent from the
shipped library, so removing the dependency costs nothing.

## Then generate with component-build arguments

```
gn gen out/Default --args="is_debug=false target_os=\"…\" target_cpu=\"…\"
                           is_component_build=true
                           rtc_enable_symbol_export=true
                           rtc_include_tests=false
                           rtc_build_tools=false
                           rtc_build_examples=false"
```

`is_component_build=true` produces the shared library. `rtc_enable_symbol_export=true` is what puts
symbols in its export table — without it the library exists but exports nothing usable.

## Same edits, three dialects

The edits are identical; only the tool differs.

**Linux** — GNU sed:

```bash
sed -i 's/rtc_static_library/rtc_shared_library/g' BUILD.gn
sed -i '/complete_static_lib/d' BUILD.gn
sed -i 's/!build_with_chromium && is_component_build/false/g' webrtc.gni
sed -i '/:frame_analyzer/d' rtc_tools/BUILD.gn
```

**macOS** — BSD sed, where `-i` requires a backup suffix:

```bash
sed -i '' 's/rtc_static_library/rtc_shared_library/g' BUILD.gn
sed -i '' '/complete_static_lib/d' BUILD.gn
sed -i '' 's/!build_with_chromium && is_component_build/false/g' webrtc.gni
sed -i '' '/:frame_analyzer/d' rtc_tools/BUILD.gn
```

> **The empty `''` is not optional.** Without it BSD sed takes the next argument as the backup
> suffix, and the edit lands somewhere unintended. This is the classic way to break the macOS
> shared build, and it fails quietly.

**Windows** — PowerShell:

```powershell
(Get-Content BUILD.gn).replace('rtc_static_library', 'rtc_shared_library') | Set-Content BUILD.gn
(Get-Content BUILD.gn) -notmatch 'complete_static_lib' | Set-Content BUILD.gn
(Get-Content webrtc.gni).replace('!build_with_chromium && is_component_build', 'false') | Set-Content webrtc.gni
(Get-Content rtc_tools\BUILD.gn) -notmatch ':frame_analyzer' | Set-Content rtc_tools\BUILD.gn
```

`.replace()` is the .NET string method — ordinal, not a regex, so `!` and `&&` need no escaping.
`-notmatch` filters the array of lines, which is the PowerShell equivalent of `sed '/…/d'`.

## Assertions

A find-and-replace that finds nothing does not fail: `sed` exits 0, and the build carries on to
produce a static library that will not load from C#. That failure would surface an hour later, or
worse, at run time in an application.

So each workflow asserts every anchor exists **before** editing:

```bash
assert_contains BUILD.gn           'rtc_static_library'
assert_contains BUILD.gn           'complete_static_lib'
assert_contains webrtc.gni         '!build_with_chromium && is_component_build'
assert_contains rtc_tools/BUILD.gn ':frame_analyzer'
```

A missing anchor stops the run immediately with a message saying the patch needs updating for that
WebRTC branch. That is the intended signal when upstream refactors: fix the patch, do not remove
the assertion.

## Verifying the result

**Linux**

```bash
file libwebrtc.so                       # expect: ELF shared object
nm -D --defined-only libwebrtc.so | wc -l   # expect: many, not zero
```

**macOS**

```bash
file libwebrtc.dylib                    # expect: Mach-O dynamically linked shared library
lipo -info libwebrtc.dylib
nm -gU libwebrtc.dylib | head
```

**Windows**

```powershell
dumpbin /exports webrtc.dll | Select-Object -First 40
```

An empty export table means edit 3 or `rtc_enable_symbol_export` did not take effect.

## Keeping it working across branches

The patch is deliberately narrow — four anchors — so a break is easy to diagnose. Verify against a
new branch before assuming:

```bash
B=refs/branch-heads/7977
curl -s "https://webrtc.googlesource.com/src/+/$B/webrtc.gni?format=TEXT" | base64 -d \
  | grep -n 'build_with_chromium && is_component_build'
curl -s "https://webrtc.googlesource.com/src/+/$B/BUILD.gn?format=TEXT" | base64 -d \
  | grep -c 'rtc_static_library'
```

All four anchors are present in `branch-heads/7977` (Chromium M152).

## Credit

The technique comes from [webrtc-sdk/libwebrtc](https://github.com/webrtc-sdk/libwebrtc); see
`WebRtcInterop/NOTICE`.
