#!/usr/bin/env python3
"""package-windows.py - stage the Windows x64 M150 artifacts.

usage: package-windows.py <webrtc_src> <build_dir> <stage_dir> <out_dir> [extra.obj ...]

  <webrtc_src>  the patched checkout (headers are taken from it)
  <build_dir>   the GN output dir, e.g. <src>/out/win-x64 (holds obj/webrtc.lib)
  <stage_dir>   receives webrtc.lib + include/ (what m150/gates.sh windows scans)
  <out_dir>     receives the release files: webrtc.lib, webrtc-headers.zip
                (BUILDINFO.json / PINS.txt / SHA256SUMS are added by the workflow)

webrtc.lib is the `webrtc` rtc_static_library (complete_static_lib = true: it
already contains BoringSSL, Opus, abseil, libyuv, ... - one file to link).
The `webrtc` target does NOT depend on api/crypto:frame_crypto_transformer
(P1/P3 code, only pulled in by the Android/iOS SDK targets), so that library
is passed as an object file [extra] (relative to <build_dir>; its .lib is a thin archive, which lib.exe cannot read) and merged into
webrtc.lib with the MSVC lib.exe.
The header tree is the public include set a native client needs: WebRTC's own
api/, rtc_base/, modules/, ... plus abseil, libyuv and BoringSSL headers.
No absolute paths, timestamps or runner data are written into the archive.
"""
import glob
import os
import shutil
import subprocess
import sys
import zipfile

ROOT_DIRS = ["api", "audio", "call", "common_audio", "common_video", "logging",
             "media", "modules", "p2p", "pc", "rtc_base", "system_wrappers", "video"]
THIRD_PARTY = ["third_party/abseil-cpp/absl", "third_party/libyuv/include",
               "third_party/boringssl/src/include"]
EXTS = (".h", ".hpp", ".inc")


def want(rel):
    parts = rel.split("/")
    if any(p in ("test", "testing", "fuzzers") for p in parts[:-1]):
        return False
    base = parts[-1]
    return base.endswith(EXTS) and "_unittest" not in base and "_fuzzer" not in base


def collect(src):
    files = []
    for d in ROOT_DIRS + THIRD_PARTY:
        top = os.path.join(src, d)
        if not os.path.isdir(top):
            if d in ROOT_DIRS:
                print("::error::package-windows: missing header dir %s" % d, file=sys.stderr)
                sys.exit(1)
            continue
        for dp, dn, fn in os.walk(top):
            dn.sort()
            for f in sorted(fn):
                rel = os.path.relpath(os.path.join(dp, f), src).replace(os.sep, "/")
                if want(rel):
                    files.append(rel)
    return sorted(files)


def main():
    if len(sys.argv) < 5:
        print(__doc__, file=sys.stderr)
        return 2
    src, build, stage, out = sys.argv[1:5]
    extras = [os.path.join(build, e) for e in sys.argv[5:]]
    lib = os.path.join(build, "obj", "webrtc.lib")
    if not os.path.isfile(lib) or os.path.getsize(lib) < 10_000_000:
        print("::error::package-windows: %s missing or implausibly small" % lib, file=sys.stderr)
        return 1
    if extras:
        for e in extras:
            if not os.path.isfile(e):
                print("::error::package-windows: extra library %s missing" % e, file=sys.stderr)
                return 1
        merged = os.path.join(build, "obj", "webrtc-merged.lib")
        # Chromium's clang package has no llvm-lib.exe, and lld-link /lib
        # crashes on this archive, so use the MSVC lib.exe of the runner's
        # Visual Studio (the same tool the build itself uses for LIB steps).
        vs = os.environ.get("GYP_MSVS_OVERRIDE_PATH", "C:/Program Files/Microsoft Visual Studio/2022/Enterprise")
        cands = sorted(glob.glob(os.path.join(vs, "VC", "Tools", "MSVC", "*", "bin", "Hostx64", "x64", "lib.exe")))
        if not cands:
            print("::error::package-windows: no MSVC lib.exe under %s" % vs, file=sys.stderr)
            return 1
        cmd = [cands[-1], "/nologo"]
        subprocess.run(cmd + ["/OUT:" + merged, lib] + extras, check=True)
        lib = merged
    files = collect(src)
    if len(files) < 1000:
        print("::error::package-windows: only %d headers found - wrong checkout?" % len(files), file=sys.stderr)
        return 1
    os.makedirs(stage, exist_ok=True)
    os.makedirs(out, exist_ok=True)
    # NB: the file name is webrtc.lib whether or not extras were merged in.
    shutil.copyfile(lib, os.path.join(stage, "webrtc.lib"))
    shutil.copyfile(lib, os.path.join(out, "webrtc.lib"))
    inc = os.path.join(stage, "include")
    zpath = os.path.join(out, "webrtc-headers.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for rel in files:
            dst = os.path.join(inc, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(os.path.join(src, rel), dst)
            zi = zipfile.ZipInfo("include/" + rel, date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            with open(os.path.join(src, rel), "rb") as fh:
                zf.writestr(zi, fh.read())
    print("package-windows: webrtc.lib %d bytes, %d headers, zip %d bytes" %
          (os.path.getsize(lib), len(files), os.path.getsize(zpath)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
