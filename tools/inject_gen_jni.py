#!/usr/bin/env python3
"""Put org/jni_zero/GEN_JNI.class into libwebrtc.aar's classes.jar.

dist_jar("libwebrtc") sets use_unprocessed_jars = true, so it merges each
dependency's .javac.jar. For a library whose only input is a srcjar -- which is
what the registration library added by add_generated_jni_to_aar.py is -- that
jar comes out empty. The class is compiled, but into .turbine.jar, and never
reaches the archive. The generated *Jni classes then call a GEN_JNI that is not
there and the app dies at startup:

    java.lang.NoClassDefFoundError: Failed resolution of: Lorg/jni_zero/GEN_JNI;

The turbine jar cannot simply be copied out. It is a header-only jar: besides
the native declarations, which legitimately have no bodies, it also strips the
implicit default constructor, and R8 rejects that:

    Absent Code attribute in method that is not native or abstract,
    position: Lorg/jni_zero/GEN_JNI;-><init>()V

So this compiles the generated source instead, which yields a real <init>.

Run it after build_aar.py, against the same --build-dir:

    python3 tools/inject_gen_jni.py --aar libwebrtc.aar --build-dir out_aar
"""

import argparse
import io
import os
import subprocess
import sys
import tempfile
import zipfile

CLASS_NAME = "org/jni_zero/GEN_JNI.class"
SOURCE_TAIL = os.path.join(
    "libwebrtc_jni_registration_java", "generated_java", "input_srcjars",
    "org", "jni_zero", "GEN_JNI.java")


def find_source(build_dir):
    """The generated source is per-architecture; any of them will do."""
    found = []
    for root, _dirs, _files in os.walk(build_dir):
        candidate = os.path.join(root, SOURCE_TAIL)
        if os.path.isfile(candidate):
            found.append(candidate)
    if not found:
        raise SystemExit(
            "No GEN_JNI.java under %s. Either the registration library is not in "
            "the dist_jar, or the build ran with multiplexing on, in which case "
            "the registry is emitted as J/N instead. Check that build_aar.py was "
            "given enable_jni_multiplexing=false use_hashed_jni_names=false."
            % build_dir)
    return sorted(found)[0]


def find_javac(src_root):
    bundled = os.path.join(src_root, "third_party", "jdk", "current", "bin", "javac")
    if os.path.isfile(bundled):
        return bundled
    from shutil import which
    javac = which("javac")
    if not javac:
        raise SystemExit("No javac: neither third_party/jdk nor one on PATH.")
    return javac


def inject(aar_path, class_bytes):
    with zipfile.ZipFile(aar_path) as aar:
        entries = {name: aar.read(name) for name in aar.namelist()}

    with zipfile.ZipFile(io.BytesIO(entries["classes.jar"])) as classes:
        inner = {name: classes.read(name) for name in classes.namelist()}

    inner[CLASS_NAME] = class_bytes

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as classes:
        for name in sorted(inner):
            classes.writestr(name, inner[name])
    entries["classes.jar"] = buf.getvalue()

    tmp = aar_path + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as out:
        for name, data in entries.items():
            out.writestr(name, data)
    os.replace(tmp, aar_path)
    return len(inner)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aar", required=True, help="the archive to patch")
    parser.add_argument("--build-dir", required=True,
                        help="the --build-dir given to build_aar.py")
    parser.add_argument("--src-root", default=".",
                        help="WebRTC checkout root, for its bundled JDK")
    parser.add_argument("--java-release", default="21",
                        help="javac --release level. Must match what the rest "
                             "of the archive was built at, and must be one the "
                             "consuming toolchain can read: .NET Android's "
                             "javac rejects anything above 21.")
    args = parser.parse_args(argv)

    source = find_source(args.build_dir)
    print("generated source: %s" % source)

    javac = find_javac(args.src_root)
    print("javac: %s" % javac)

    with tempfile.TemporaryDirectory() as work:
        # --release matters as much here as it does for the rest of the
        # archive. Without it javac emits the bundled JDK's default, which is
        # how one major-69 class ended up in an otherwise major-65 AAR -- a
        # difference nothing noticed until a consumer tried to read that
        # particular class.
        subprocess.run([javac, "--release", args.java_release,
                        "-nowarn", "-d", work, source], check=True)
        compiled = os.path.join(work, CLASS_NAME)
        if not os.path.isfile(compiled):
            raise SystemExit("javac produced no %s" % CLASS_NAME)
        class_bytes = open(compiled, "rb").read()
    print("compiled GEN_JNI.class: %d bytes" % len(class_bytes))

    count = inject(args.aar, class_bytes)
    print("classes.jar now holds %d entries, including %s" % (count, CLASS_NAME))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
