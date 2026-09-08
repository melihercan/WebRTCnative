#!/usr/bin/env python3
"""Add the generated JNI java targets to the AAR's class list.

WebRTC builds libwebrtc.aar's classes.jar from the dist_jar("libwebrtc") target
in sdk/android/BUILD.gn. That target sets direct_deps_only = true, so only the
jars of the targets it lists itself are merged -- not their dependencies.

Since the move to jni_zero, the JNI glue no longer lives in the java libraries.
It is generated into separate generated_*_jni_java targets which those libraries
depend on, and which the dist_jar does not list. The classes therefore never
reach classes.jar, and an app using the AAR dies on startup:

    java.lang.NoClassDefFoundError:
        Failed resolution of: Lorg/webrtc/PeerConnectionFactoryJni;
      at org.webrtc.PeerConnectionFactory.initialize(PeerConnectionFactory.java:328)

That is only half of it. Every generated *Jni class calls into org.jni_zero.GEN_JNI,
jni_zero's per-binary registry of native methods, which is produced by the
implicit libjingle_peerconnection_so__jni_registration target -- rtc_shared_library
is shared_library_with_jni on Android, so that sub-target exists. Nothing packages
it either, so shipping the *Jni classes alone just moves the crash one class along:

    java.lang.ClassNotFoundException: Didn't find class "org.jni_zero.GEN_JNI"

This adds both: a small library carrying the registration srcjar, and the
generated JNI targets, all wired into the dist_jar. The two test-only generators
are left out; an AAR for distribution has no use for them.

Run it against a WebRTC checkout, or against a copy of sdk/android/BUILD.gn to
test it off CI:

    python3 tools/add_generated_jni_to_aar.py path/to/sdk/android/BUILD.gn
"""

import argparse
import re
import sys

# Generators for WebRTC's own test binaries, not part of the distributed API.
# The library carrying GEN_JNI. Declared here rather than reusing an existing java
# target: the registration walks the shared library's dependency closure, and
# feeding its srcjar back into a library that closure contains would be a cycle.
REGISTRATION_LIBRARY = """  rtc_android_library("libwebrtc_jni_registration_java") {
    srcjar_deps = [ ":libjingle_peerconnection_so__jni_registration" ]
    deps = [ "//third_party/jni_zero:jni_zero_java" ]
  }

"""

REGISTRATION_TARGET = "libwebrtc_jni_registration_java"

TEST_ONLY = {
    "generated_instrumentationtests_jni_java",
    "generated_native_unittests_jni_java",
}


def patch(text):
    """Return the patched BUILD.gn, and the targets that were added."""
    match = re.search(r'dist_jar\("libwebrtc"\)\s*\{.*?\n  \}\n', text, re.S)
    if not match:
        raise SystemExit(
            'Could not find the dist_jar("libwebrtc") target in sdk/android/BUILD.gn. '
            "The AAR is assembled somewhere else now; this patch needs updating."
        )

    block = match.group(0)
    if "direct_deps_only = true" not in block:
        raise SystemExit(
            "dist_jar(\"libwebrtc\") no longer sets direct_deps_only. Check whether it "
            "now picks the generated JNI classes up on its own before deleting this step."
        )

    wanted = sorted(set(re.findall(r'":(generated_\w+_jni_java)"', text)) - TEST_ONLY)
    if "generated_peerconnection_jni_java" not in wanted:
        raise SystemExit(
            "No generated_peerconnection_jni_java target found. The generated JNI "
            "targets have been renamed and this patch needs updating."
        )

    wanted.append(REGISTRATION_TARGET)

    missing = [t for t in wanted if '":%s"' % t not in block]
    if not missing:
        return text, []

    # gn format keeps a deps list alphabetical, so merge rather than prepend:
    # appending at the top would make every patched build fail a format check.
    local = re.findall(r'^      ":[^"]+",$', block, re.M)
    merged = sorted(set(local) | {'      ":%s",' % t for t in missing})
    if not local:
        raise SystemExit(
            "The dist_jar target has no recognisable deps list; this patch needs updating."
        )
    joiner = "\n"
    patched_block = block.replace(joiner.join(local), joiner.join(merged), 1)
    if patched_block == block:
        raise SystemExit(
            "The dist_jar target has no recognisable deps list; this patch needs updating."
        )

    patched = text.replace(block, patched_block, 1)

    # Declare the registration library just above the dist_jar that consumes it.
    if 'rtc_android_library("%s")' % REGISTRATION_TARGET not in patched:
        patched = patched.replace(
            '  dist_jar("libwebrtc") {', REGISTRATION_LIBRARY + '  dist_jar("libwebrtc") {', 1
        )

    return patched, missing


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "build_gn",
        nargs="?",
        default="sdk/android/BUILD.gn",
        help="path to sdk/android/BUILD.gn (default: %(default)s)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="report what would change without writing",
    )
    args = parser.parse_args(argv)

    with open(args.build_gn, encoding="utf-8") as handle:
        original = handle.read()

    patched, added = patch(original)

    if not added:
        print("Already patched; the dist_jar lists every generated JNI target.")
        return 0

    if not args.check:
        with open(args.build_gn, "w", encoding="utf-8") as handle:
            handle.write(patched)

    print("Added %d generated JNI java targets to dist_jar(\"libwebrtc\"):" % len(added))
    for target in added:
        print("  %s" % target)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
