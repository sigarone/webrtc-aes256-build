#!/usr/bin/env python3
"""package-windows.py - stage the Windows x64 M150 artifacts.

usage: package-windows.py <webrtc_src> <build_dir> <stage_dir> <out_dir> [--libcxx] [extra.obj ...]

  <webrtc_src>  the patched checkout (headers are taken from it)
  <build_dir>   the GN output dir, e.g. <src>/out/win-x64 (holds obj/webrtc.lib)
  <stage_dir>   receives webrtc.lib + include/ (what m150/gates.sh windows scans)
  <out_dir>     receives the release files: webrtc.lib, webrtc-headers.zip and,
                with --libcxx, libcxx.lib
                (build-flags.json / BUILDINFO.json / PINS.txt / SHA256SUMS are
                added by the workflow)
  --libcxx      the library was built against Chromium's libc++ (GN default on
                Windows with clang-cl). webrtc.lib does NOT carry the libc++
                runtime objects (they are a GN source_set that only executables
                pull in), so a consumer cannot link without them: this flag
                merges the objects of //buildtools/third_party/libc++:libc++
                (and its llvm-libc-shared dep) into libcxx.lib and adds the
                libc++ headers + __config_site to webrtc-headers.zip.
                The caller must have built that GN target in <build_dir>.

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
import re
import shutil
import subprocess
import sys
import zipfile

ROOT_DIRS = ["api", "audio", "call", "common_audio", "common_video", "logging",
             "media", "modules", "p2p", "pc", "rtc_base", "system_wrappers", "video"]
THIRD_PARTY = ["third_party/abseil-cpp/absl", "third_party/libyuv/include",
               "third_party/boringssl/src/include"]
EXTS = (".h", ".hpp", ".inc")


LIBCXX_OBJ_DIRS = ["obj/buildtools/third_party/libc++/libc++",
                   "obj/third_party/llvm-libc/llvm-libc-shared"]
LIBCXX_REQUIRED_OBJS = ["string.obj", "locale.obj", "ios.obj", "thread.obj",
                        "hash.obj", "memory.obj", "mutex.obj"]
LIBCXX_HEADER_DIR = "third_party/libc++/src/include"
LIBCXX_CONFIG_DIR = "buildtools/third_party/libc++"
LIBCXX_CONFIG_FILES = ["__config_site", "__assertion_handler"]


def msvc_lib_exe():
    # Chromium's clang package has no llvm-lib.exe, and lld-link /lib
    # crashes on this archive, so use the MSVC lib.exe of the runner's
    # Visual Studio (the same tool the build itself uses for LIB steps).
    vs = os.environ.get("GYP_MSVS_OVERRIDE_PATH", "C:/Program Files/Microsoft Visual Studio/2022/Enterprise")
    cands = sorted(glob.glob(os.path.join(vs, "VC", "Tools", "MSVC", "*", "bin", "Hostx64", "x64", "lib.exe")))
    if not cands:
        print("::error::package-windows: no MSVC lib.exe under %s" % vs, file=sys.stderr)
        sys.exit(1)
    return cands[-1]


def libcxx_objects(build):
    objs = []
    for sub in LIBCXX_OBJ_DIRS:
        top = os.path.join(build, sub)
        if not os.path.isdir(top):
            continue
        for dp, dn, fn in os.walk(top):
            dn.sort()
            for f in sorted(fn):
                if f.endswith(".obj"):
                    objs.append(os.path.join(dp, f))
    names = {os.path.basename(o) for o in objs}
    missing = [n for n in LIBCXX_REQUIRED_OBJS if n not in names]
    if len(objs) < 40 or missing:
        print("::error::package-windows: libc++ objects incomplete (%d found, missing %s) - "
              "was //buildtools/third_party/libc++:libc++ built?" % (len(objs), missing), file=sys.stderr)
        sys.exit(1)
    return objs


def libcxx_headers(src):
    files = []
    top = os.path.join(src, LIBCXX_HEADER_DIR)
    if not os.path.isdir(top):
        print("::error::package-windows: %s missing" % LIBCXX_HEADER_DIR, file=sys.stderr)
        sys.exit(1)
    for dp, dn, fn in os.walk(top):
        dn.sort()
        for f in sorted(fn):
            files.append(os.path.relpath(os.path.join(dp, f), src).replace(os.sep, "/"))
    for f in LIBCXX_CONFIG_FILES:
        rel = LIBCXX_CONFIG_DIR + "/" + f
        if not os.path.isfile(os.path.join(src, rel)):
            print("::error::package-windows: %s missing" % rel, file=sys.stderr)
            sys.exit(1)
        files.append(rel)
    if len(files) < 500:
        print("::error::package-windows: only %d libc++ header files" % len(files), file=sys.stderr)
        sys.exit(1)
    return sorted(files)


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


INCLUDE_RE = re.compile(rb'^[ 	]*#[ 	]*include[ 	]+"([^"]+)"', re.M)


def close_headers(src, files):
    """Add every in-tree header the packaged headers (transitively) include
    but the directory walk did not carry (for example sdk/objc/base/
    RTCMacros.h, pulled in by api/audio/audio_device.h). Includes that resolve
    only through a third_party include dir (absl/, libyuv/, openssl/, ...) are
    covered by the directory walk; other third_party/ paths are reported, not
    added."""
    have = set(files)
    work = list(files)
    added = []
    third = set()
    while work:
        rel = work.pop()
        try:
            with open(os.path.join(src, rel), "rb") as fh:
                data = fh.read()
        except OSError:
            continue
        here = os.path.dirname(rel)
        for m in INCLUDE_RE.finditer(data):
            inc = m.group(1).decode("utf-8", "replace")
            if inc in have:
                continue
            cands = [inc]
            if here:
                cands.append(here + "/" + inc)
            for c in cands:
                c = os.path.normpath(c).replace(os.sep, "/")
                if c in have:
                    break
                if not c.endswith(EXTS):
                    continue
                if c.startswith("third_party/"):
                    if os.path.isfile(os.path.join(src, c)):
                        third.add(c.split("/")[1])
                    continue
                if os.path.isfile(os.path.join(src, c)):
                    have.add(c)
                    work.append(c)
                    added.append(c)
                    break
    print("package-windows: header closure added %d file(s); third_party dirs referenced but not shipped: %s"
          % (len(added), sorted(third)))
    for a in sorted(added)[:60]:
        print("  + " + a)
    return sorted(have)


def main():
    want_libcxx = "--libcxx" in sys.argv
    argv = [a for a in sys.argv[1:] if a != "--libcxx"]
    if len(argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    src, build, stage, out = argv[0:4]
    extras = [os.path.join(build, e) for e in argv[4:]]
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
        cmd = [msvc_lib_exe(), "/nologo"]
        subprocess.run(cmd + ["/OUT:" + merged, lib] + extras, check=True)
        lib = merged
    cxx_lib = None
    if want_libcxx:
        objs = libcxx_objects(build)
        cxx_lib = os.path.join(build, "obj", "libcxx.lib")
        rsp = os.path.join(build, "obj", "libcxx.rsp")
        with open(rsp, "w", encoding="utf-8") as f:
            for o in objs:
                f.write('"%s"\n' % o)
        subprocess.run([msvc_lib_exe(), "/nologo", "/OUT:" + cxx_lib, "@" + rsp], check=True)
        print("package-windows: libcxx.lib %d bytes from %d objects" % (os.path.getsize(cxx_lib), len(objs)))
    files = close_headers(src, collect(src))
    cxx_files = libcxx_headers(src) if want_libcxx else []
    if len(files) < 1000:
        print("::error::package-windows: only %d headers found - wrong checkout?" % len(files), file=sys.stderr)
        return 1
    os.makedirs(stage, exist_ok=True)
    os.makedirs(out, exist_ok=True)
    # NB: the file name is webrtc.lib whether or not extras were merged in.
    shutil.copyfile(lib, os.path.join(stage, "webrtc.lib"))
    shutil.copyfile(lib, os.path.join(out, "webrtc.lib"))
    if cxx_lib:
        shutil.copyfile(cxx_lib, os.path.join(stage, "libcxx.lib"))
        shutil.copyfile(cxx_lib, os.path.join(out, "libcxx.lib"))
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
        # Chromium's libc++ headers (no file extension, so collect() cannot
        # see them) and the two config files of the checkout. Zip only:
        # nothing in the gates reads them, so they are not staged.
        for rel in cxx_files:
            zi = zipfile.ZipInfo("include/" + rel, date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            with open(os.path.join(src, rel), "rb") as fh:
                zf.writestr(zi, fh.read())
    print("package-windows: webrtc.lib %d bytes, %d headers (+%d libc++), zip %d bytes" %
          (os.path.getsize(lib), len(files), len(cxx_files), os.path.getsize(zpath)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
