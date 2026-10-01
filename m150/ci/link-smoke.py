#!/usr/bin/env python3
"""link-smoke.py - compile, link and run the out-of-tree consumer (S0.2).

usage: link-smoke.py <artifact_dir> <clang_root> <work_dir> <consumer.cc>

  <artifact_dir>  the release files exactly as a consumer downloads them:
                  webrtc.lib, libcxx.lib, webrtc-headers.zip, build-flags.json
  <clang_root>    the Chromium clang package named by build-flags.json
                  (bin/clang-cl.exe, bin/lld-link.exe, lib/clang/..)
  <work_dir>      scratch directory (headers are unpacked here)
  <consumer.cc>   the program to build

The ONLY inputs of the compile and link are the four release files plus the
runner's own MSVC / Windows SDK environment (INCLUDE / LIB, imported from
vcvars64.bat unless already set). No path or flag is taken from the GN build tree.

exit: 0 the program printed SMOKE-OK and returned 0 | 1 any failure
"""
import json
import os
import subprocess
import sys
import zipfile


def die(msg):
    print("::error::link-smoke: " + msg, file=sys.stderr)
    sys.exit(1)


def run(cmd, **kw):
    print("+ " + " ".join('"%s"' % c if " " in c else c for c in cmd), flush=True)
    return subprocess.run(cmd, **kw)


VCVARS = "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build/vcvars64.bat"


def import_msvc_environment():
    """INCLUDE / LIB / PATH of the runner's MSVC + Windows SDK (what vcvars64
    sets). Skipped when the caller already runs inside such an environment."""
    if os.environ.get("INCLUDE") and os.environ.get("LIB"):
        return
    if not os.path.isfile(VCVARS):
        die("no vcvars64.bat at the expected location and INCLUDE/LIB not set")
    bat = VCVARS.replace("/", "\\")
    r = subprocess.run('cmd /c ""%s" >nul && set"' % bat, shell=True,
                       capture_output=True, text=True)
    if r.returncode != 0:
        die("vcvars64.bat failed")
    for line in r.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            os.environ[k] = v
    if not os.environ.get("INCLUDE") or not os.environ.get("LIB"):
        die("INCLUDE/LIB still empty after vcvars64")


def main():
    if len(sys.argv) != 5:
        print("link-smoke: expected 4 arguments, got %d: %r" % (len(sys.argv) - 1, sys.argv[1:]),
              file=sys.stderr)
        print(__doc__, file=sys.stderr)
        return 2
    art, clang, work, src = [os.path.abspath(a) for a in sys.argv[1:5]]
    for need in ("webrtc.lib", "libcxx.lib", "webrtc-headers.zip", "build-flags.json"):
        if not os.path.isfile(os.path.join(art, need)):
            die("release file missing: " + need)
    import_msvc_environment()
    flags = json.load(open(os.path.join(art, "build-flags.json"), encoding="utf-8"))
    if flags.get("schema") != "qaudion-webrtc-buildflags/1":
        die("unexpected build-flags schema")

    hdr = os.path.join(work, "hdr")
    os.makedirs(work, exist_ok=True)
    with zipfile.ZipFile(os.path.join(art, "webrtc-headers.zip")) as z:
        z.extractall(hdr)
    root = os.path.join(hdr, flags["compile"]["header_root"])
    if not os.path.isdir(root):
        die("header root %s not in the zip" % flags["compile"]["header_root"])

    incs = []
    skipped = []
    for d in flags["compile"]["include_dirs"]:
        # GN wrote these relative to the build dir: "../.." is the source root
        if d == "../..":
            p = root
        elif d.startswith("../../"):
            p = os.path.join(root, d[len("../../"):])
        else:
            skipped.append(d)
            continue
        if os.path.isdir(p):
            incs.append(p)
        else:
            skipped.append(d)
    # BoringSSL headers are public API surface of the library (SSL_* ids) and
    # ship in the zip next to everything else.
    bssl = os.path.join(root, "third_party", "boringssl", "src", "include")
    if os.path.isdir(bssl) and bssl not in incs:
        incs.append(bssl)
    print("link-smoke: include dirs used: %d, not shipped/not needed: %s" % (len(incs), skipped))
    if not any(i.replace("\\", "/").endswith("third_party/libc++/src/include") for i in incs) \
            and "libc++" in flags["abi"]["stl"]:
        die("libc++ headers are not in the header zip")

    rsp = os.path.join(work, "compile.rsp")
    with open(rsp, "w", encoding="utf-8") as f:
        for d in flags["compile"]["defines"]:
            f.write("-D%s\n" % d)
        for i in incs:
            f.write('-I"%s"\n' % i.replace("\\", "/"))
        for fl in flags["compile"]["flags"]:
            f.write("%s\n" % fl)

    obj = os.path.join(work, "consumer.obj")
    exe = os.path.join(work, "smoke.exe")
    cl = os.path.join(clang, "bin", "clang-cl.exe")
    link = os.path.join(clang, "bin", "lld-link.exe")
    for tool in (cl, link):
        if not os.path.isfile(tool):
            die("missing tool " + tool)

    r = run([cl, "/nologo", "/c", src, "/Fo" + obj, "/O1", "-Wno-everything", "@" + rsp])
    if r.returncode != 0:
        die("COMPILE failed (headers + build-flags.json do not build a consumer)")

    builtins = os.path.join(clang, flags["link"]["compiler_rt_builtins"].replace("/", os.sep))
    if not os.path.isfile(builtins):
        die("compiler-rt builtins not found: " + flags["link"]["compiler_rt_builtins"])
    libs = [os.path.join(art, l) for l in flags["link"]["static_libs"]]
    libs += flags["link"]["system_libs"]
    libs += [builtins]
    cmd = [link, "/nologo", "/OUT:" + exe, "/MACHINE:X64", "/SUBSYSTEM:CONSOLE", obj] + libs
    for dl in flags["link"]["default_libs"]:
        cmd.append("/DEFAULTLIB:" + dl)
    r = run(cmd)
    if r.returncode != 0:
        die("LINK failed (webrtc.lib + libcxx.lib + system libs do not link a consumer)")

    r = run([exe], capture_output=True, text=True, timeout=240)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr[-4000:])
    if r.returncode != 0 or "[smoke] SMOKE-OK" not in r.stdout:
        die("the program did not pass (exit %d)" % r.returncode)
    print("link-smoke: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
