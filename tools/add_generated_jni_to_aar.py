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

This adds those targets to the dist_jar so the generated classes ship with the
API that calls them. The two test-only generators are left out; an AAR for
distribution has no use for them.

Run it against a WebRTC checkout, or against a copy of sdk/android/BUILD.gn to
test it off CI:

    python3 tools/add_generated_jni_to_aar.py path/to/sdk/android/BUILD.gn
"""

import argparse
import re
import sys

# Generators for WebRTC's own test binaries, not part of the distributed API.
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

    missing = [t for t in wanted if '":%s"' % t not in block]
    if not missing:
        return text, []

    added = "".join('      ":%s",\n' % target for target in missing)
    patched_block = block.replace("    deps = [\n", "    deps = [\n" + added, 1)
    if patched_block == block:
        raise SystemExit(
            "The dist_jar target has no recognisable deps list; this patch needs updating."
        )

    return text.replace(block, patched_block, 1), missing


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
