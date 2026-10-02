#!/usr/bin/env python3
"""link-smoke.py - compile, link and run the out-of-tree consumer (S0.2).

usage: link-smoke.py <artifact_dir> <clang_root> <work_dir> <consumer.cc>
       link-smoke.py --clang-package <build-flags.json>

The second form validates the compiler package url and sha256 named by
build-flags.json (full-string match, one line each) and prints "<url> <sha256>"
on one line; the workflow downloads the package from exactly that.

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
import re
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


DEFINE_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(=[A-Za-z0-9_.:+-]*)?")
FLAG_RE = re.compile(r"(/[A-Za-z][A-Za-z0-9:_+.=-]*|-[A-Za-z][A-Za-z0-9_+.=-]*)")
ALLOWED_FLAG_PREFIXES = ("/std:", "-std=", "-fmsc-version=", "-m64", "-m32", "-msse", "-mavx",
                         "/arch:", "/MT", "/MD", "/utf-8", "/Zc:", "/Zp", "/GR", "/EH",
                         "-fcomplete-member-pointers", "-fno-exceptions", "-fexceptions",
                         "-fno-rtti", "-frtti", "-fms-compatibility")

# Every path that build-flags.json hands to a command line or to os.path.join is
# validated SEGMENT by SEGMENT (split on "/"), never with one regex over the whole
# string: a regex over the string let "../..//Windows/System32" through (the "//"
# made os.path.join restart at an absolute path on Windows) and accepted a lib
# name starting with "-" or "+" (an option for the linker).
#
# A plain segment: starts with a letter, a digit or "_" (so it is never ".", "..",
# an option such as "-x" / "+x", or "@file"), then only [A-Za-z0-9_.+-] (so no
# ":" = no drive letter or NTFS stream, no backslash, no space, no quote).
SEGMENT_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.+-]*")
LIB_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.+-]*\.lib")   # one segment: a file name, never a path


def plain_segments(segments):
    """True when every segment is plain; an empty segment ("//"), "." and ".."
    are not."""
    return all(SEGMENT_RE.fullmatch(s) for s in segments)


def is_plain_rel_path(value):
    """A relative path of plain segments, no leading "/", no empty, "." or ".."
    segment, no drive, no backslash."""
    return isinstance(value, str) and plain_segments(value.split("/"))


def is_include_dir(value):
    """An include dir as GN writes it relative to the build dir: "../.." (the
    source root), "../../<plain path>", or "gen"/"gen/<plain path>". The ONLY ".."
    segments allowed are the two leading ones."""
    if not isinstance(value, str):
        return False
    segs = value.split("/")
    if segs[:2] == ["..", ".."]:
        return plain_segments(segs[2:])
    return segs[0] == "gen" and plain_segments(segs)


CLANG_URL_RE = re.compile(
    r"https://commondatastorage\.googleapis\.com/chromium-browser-clang/Win/clang-[A-Za-z0-9._-]+\.tar\.xz")
SHA256_RE = re.compile(r"[0-9a-f]{64}")


def safe_rel(value, what):
    """build-flags.json paths that are joined to a directory of ours must stay
    inside it."""
    if not is_plain_rel_path(value):
        die("build-flags.json %s is not a plain relative path: %r" % (what, value))
    return value


def clang_package(flags):
    """(url, sha256) of the compiler package, validated with a FULL match (re
    anchors with ^/$ would accept a multi-line value through any one line)."""
    tc = flags.get("toolchain", {})
    url, sha = tc.get("clang_package_url"), tc.get("clang_package_sha256")
    if not isinstance(url, str) or not CLANG_URL_RE.fullmatch(url):
        die("unexpected clang package url in build-flags.json")
    if not isinstance(sha, str) or not SHA256_RE.fullmatch(sha):
        die("build-flags.json carries no valid clang package sha256")
    return url, sha


def validate_flags(flags):
    """build-flags.json is data produced by an earlier job: only plain tokens
    may reach the compiler and linker command lines (no paths outside the
    unpacked headers, no /FORCE or /LIBPATH style options, no response files)."""
    bad = []
    for d in flags["compile"]["defines"]:
        if not DEFINE_RE.fullmatch(d):
            bad.append(("define", d))
    for f in flags["compile"]["flags"]:
        if not FLAG_RE.fullmatch(f) or not f.startswith(ALLOWED_FLAG_PREFIXES):
            bad.append(("flag", f))
    for l in flags["link"]["system_libs"] + flags["link"]["static_libs"] + flags["link"]["default_libs"]:
        if not isinstance(l, str) or not LIB_RE.fullmatch(l):
            bad.append(("lib", l))
    for i in flags["compile"]["include_dirs"]:
        if not is_include_dir(i):
            bad.append(("include dir", i))
    if bad:
        die("build-flags.json carries tokens that are not plain compile/link options: %r" % (bad[:8],))
    safe_rel(flags["compile"]["header_root"], "compile.header_root")
    if not safe_rel(flags["link"]["compiler_rt_builtins"], "link.compiler_rt_builtins").endswith(
            "clang_rt.builtins-x86_64.lib"):
        die("link.compiler_rt_builtins is not the x64 compiler-rt builtins library")
    clang_package(flags)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--clang-package":
        url, sha = clang_package(json.load(open(sys.argv[2], encoding="utf-8")))
        print("%s %s" % (url, sha))
        return 0
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
    validate_flags(flags)

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

    # Own deadline: a hang must still show everything the program printed.
    print("+ " + exe, flush=True)
    p = subprocess.Popen([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors="replace")
    timed_out = False
    try:
        out, _ = p.communicate(timeout=150)
    except subprocess.TimeoutExpired:
        timed_out = True
        p.kill()
        out, _ = p.communicate()
    lines = out.splitlines()
    # smoke verdict lines first, then the tail of the library log
    for l in lines:
        if l.startswith("[smoke]"):
            print(l)
    tail = [l for l in lines if not l.startswith("[smoke]")][-80:]
    if tail:
        print("--- library log (last %d lines) ---" % len(tail))
        for l in tail:
            print(l)
    if timed_out:
        die("the program hung (killed after 150 s)")
    if p.returncode != 0 or "[smoke] SMOKE-OK" not in [l.rstrip() for l in lines]:
        die("the program did not pass (exit %d)" % p.returncode)
    print("link-smoke: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
