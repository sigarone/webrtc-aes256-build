#!/usr/bin/env python3
"""gen-build-flags.py - build-flags.json for the Windows M150 library (S0.1).

usage: gen-build-flags.py <raw_dir> <gn_args> > build-flags.json

<raw_dir> holds the raw facts the workflow collected from the REAL build (same
GN output directory the library was compiled in, no hand-written values):

  desc-webrtc.json   gn desc <out> //:webrtc --format=json
  compdb.json        ninja -C <out> -t compdb cxx   (the real compiler command
                     lines; the command of a webrtc translation unit is the
                     source of defines, include dirs and ABI-relevant flags)
  clang-version.txt  <clang-cl> --version
  cr-build-revision  third_party/llvm-build/Release+Asserts/cr_build_revision
  clang-sha256       sha256 of the clang package tarball (single line, required)

The file tells a consumer exactly how to compile and link against webrtc.lib:
defines, include directories (as GN wrote them, relative to the build dir; the
header zip is rooted so that "../../X" is "<headers>/include/X"), the ABI
flags, the C++ standard, the STL in use (and, for Chromium's libc++, the
revision and what must be linked next to webrtc.lib), the CRT mode, RTTI,
exceptions, the toolchain versions and the system libraries.

Nothing in the output contains an absolute runner path, a user name or a
secret. exit: 0 ok | 1 a required fact could not be derived (fail closed)
"""
import json
import os
import re
import shlex
import sys

PROBE_TU = "pc/peer_connection_factory.cc"

# Flags that change the generated code/ABI or the language mode and must be
# passed unchanged by a consumer. Everything else in the library's command line
# (warnings, hardening, plugins, optimisation, debug-info) is the library's own
# business and is deliberately not exported.
ABI_FLAGS_EXACT = {
    "/MT", "/MTd", "/MD", "/MDd", "/utf-8", "/Zc:twoPhase", "/Zc:inline",
    "-fcomplete-member-pointers", "-m64", "-m32", "/GR", "/GR-", "/EHsc",
    "/EHs", "/EHa", "/EHs-c-", "-fno-exceptions", "-fexceptions",
    "-fno-rtti", "-frtti", "/Zp8",
}
ABI_FLAG_PREFIXES = ("/std:", "-std=", "-fmsc-version=", "-fms-compatibility",
                     "/arch:", "/Zp", "-msse", "-mavx")

# Defines that only make sense while building the library itself.
DROP_DEFINES = {"WEBRTC_LIBRARY_IMPL"}
DROP_DEFINE_PREFIXES = ("CR_CLANG_REVISION=", "PROTOBUF_", "GOOGLE_PROTOBUF_",
                        "__DATE__", "__TIME__", "__TIMESTAMP__")


def die(msg):
    print("::error::gen-build-flags: " + msg, file=sys.stderr)
    sys.exit(1)


def read(path, required=True):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        if required:
            die("missing raw fact file: %s" % os.path.basename(path))
        return None


def probe_command(compdb):
    for e in compdb:
        f = e.get("file", "").replace("\\", "/")
        if f.endswith(PROBE_TU):
            return e["command"]
    die("no compile command for %s in compdb.json" % PROBE_TU)


def tokenize(command):
    # The first token is the compiler path (Windows backslashes); the rest is
    # a Windows command line whose only escape is \" inside quotes.
    _, rest = command.split(" ", 1)
    return shlex.split(rest, posix=True)


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    raw, gn_args = sys.argv[1], sys.argv[2]
    desc = json.loads(read(os.path.join(raw, "desc-webrtc.json")))
    if "//:webrtc" not in desc:
        die("gn desc has no //:webrtc entry")
    target = desc["//:webrtc"]
    compdb = json.loads(read(os.path.join(raw, "compdb.json")))
    tokens = tokenize(probe_command(compdb))

    defines = []
    includes = []
    flags = []
    msvc_toolset = None
    win_sdk = None
    for t in tokens:
        if t.startswith("-D") and len(t) > 2:
            defines.append(t[2:])
        elif t.startswith("/D") and len(t) > 2:
            defines.append(t[2:])
        elif t.startswith("-I") and len(t) > 2:
            includes.append(t[2:].replace("\\", "/"))
        elif t.startswith("-imsvc"):
            p = t[len("-imsvc"):].replace("\\", "/")
            m = re.search(r"/MSVC/([0-9.]+)/", p)
            if m:
                msvc_toolset = m.group(1)
            m = re.search(r"/include/(10\.[0-9.]+)/", p)
            if m:
                win_sdk = m.group(1)
        elif t in ABI_FLAGS_EXACT or t.startswith(ABI_FLAG_PREFIXES):
            if t not in flags:
                flags.append(t)

    defines = [d for d in defines
               if d not in DROP_DEFINES and not d.startswith(DROP_DEFINE_PREFIXES)]
    # defines the build adds through GN configs but that live on the target
    # description only (same list, kept as a cross-check)
    gn_defines = set(target.get("defines", []))
    for d in sorted(gn_defines):
        if (d not in defines and d not in DROP_DEFINES
                and not d.startswith(DROP_DEFINE_PREFIXES)):
            defines.append(d)

    if not any(d == "QAUDION_TRANSPORT_STRICT=1" for d in defines):
        die("QAUDION_TRANSPORT_STRICT=1 is not among the compile defines")

    crt = None
    for f in flags:
        if f in ("/MT", "/MTd", "/MD", "/MDd"):
            crt = f
    if crt is None:
        die("no /MT|/MD flag in the compile command - CRT mode unknown")
    std = None
    for f in flags:
        if f.startswith("/std:") or f.startswith("-std="):
            std = f.split(":", 1)[1] if f.startswith("/std:") else f.split("=", 1)[1]
    if std is None:
        die("no C++ standard flag in the compile command")

    # STL: Chromium's libc++ shows up as an include dir under third_party/libc++
    # The public headers include <openssl/...> (BoringSSL ids) and the zip
    # carries that tree; the library's own command line does not list it
    # because the library finds it through a dependency's config, so the
    # contract states it explicitly.
    bssl_inc = "../../third_party/boringssl/src/include"
    if bssl_inc not in includes:
        includes.append(bssl_inc)
    libcxx_inc = [i for i in includes if "third_party/libc++/src/include" in i]
    libcxx_cfg = [i for i in includes if i.endswith("buildtools/third_party/libc++")]
    uses_libcxx = bool(libcxx_inc)
    libcxx_rev = None
    for d in defines:
        if d.startswith("CR_LIBCXX_REVISION="):
            libcxx_rev = d.split("=", 1)[1]
    iter_level = None
    for d in defines:
        if d.startswith("_ITERATOR_DEBUG_LEVEL="):
            iter_level = int(d.split("=", 1)[1])
    hardening = None
    for d in defines:
        if d.startswith("_LIBCPP_HARDENING_MODE="):
            hardening = d.split("=", 1)[1]
    if uses_libcxx and (not libcxx_cfg or libcxx_rev is None):
        die("libc++ detected but its config dir / revision is missing")

    rtti = not ("/GR-" in flags or "-fno-rtti" in flags)
    exceptions = any(f in flags for f in ("/EHsc", "/EHs", "/EHa", "-fexceptions"))

    cr_rev = (read(os.path.join(raw, "cr-build-revision")) or "").strip()
    if not re.fullmatch(r"[A-Za-z0-9._-]+", cr_rev or ""):
        die("clang cr_build_revision missing or odd: %r" % cr_rev)
    clang_ver = (read(os.path.join(raw, "clang-version.txt")) or "").strip().splitlines()
    clang_ver_line = clang_ver[0] if clang_ver else ""
    if not clang_ver_line.startswith("clang version"):
        die("clang --version output not recognised: %r" % clang_ver_line)
    clang_sha = (read(os.path.join(raw, "clang-sha256")) or "").strip()
    if not re.fullmatch(r"[0-9a-f]{64}", clang_sha):
        die("clang package sha256 missing or malformed")
    if not msvc_toolset or not win_sdk:
        die("MSVC toolset / Windows SDK version not found in the compile command")

    libs = []
    builtins = None
    for l in target.get("libs", []):
        l = l.replace("\\", "/")
        if l.endswith("clang_rt.builtins-x86_64.lib"):
            m = re.search(r"(lib/clang/[^/]+/lib/windows/clang_rt\.builtins-x86_64\.lib)$", l)
            builtins = m.group(1) if m else None
        elif not l.startswith("//"):
            libs.append(l)
    if builtins is None:
        die("compiler-rt builtins library not in the target libs")

    default_libs = []
    for l in target.get("ldflags", []):
        m = re.fullmatch(r"/DEFAULTLIB:(.+)", l)
        if m:
            default_libs.append(m.group(1))

    static_libs = ["webrtc.lib"] + (["libcxx.lib"] if uses_libcxx else [])
    out = {
        "schema": "qaudion-webrtc-buildflags/1",
        "platform": "windows",
        "target": {"os": "win", "cpu": "x64", "triple": "x86_64-pc-windows-msvc"},
        "gn_args": gn_args,
        "toolchain": {
            "compiler": "clang-cl (Chromium package)",
            "clang_version": clang_ver_line,
            "clang_package_revision": cr_rev,
            "clang_package_url":
                "https://commondatastorage.googleapis.com/chromium-browser-clang/Win/clang-%s.tar.xz" % cr_rev,
            "clang_package_sha256": clang_sha,
            "msvc_toolset": msvc_toolset,
            "windows_sdk": win_sdk,
        },
        "abi": {
            "cxx_standard": std,
            "stl": "libc++ (Chromium, inline namespace std::__Cr)" if uses_libcxx else "MSVC STL",
            "libcxx_revision": libcxx_rev,
            "libcxx_hardening_mode": hardening,
            "iterator_debug_level": iter_level,
            "crt": {"/MT": "static release (libcmt)", "/MTd": "static debug",
                    "/MD": "dynamic release", "/MDd": "dynamic debug"}[crt],
            "crt_flag": crt,
            "rtti": rtti,
            "exceptions": exceptions,
        },
        "compile": {
            "defines": defines,
            "include_dirs": includes,
            "flags": flags,
            "header_root": "include",
            "include_dir_mapping":
                "an include dir written '../../X' by GN is <header zip>/include/X; "
                "'../..' is <header zip>/include; dirs the zip does not carry "
                "(generated files) are not needed by the public headers",
        },
        "link": {
            "static_libs": static_libs,
            "system_libs": libs,
            "default_libs": default_libs,
            "compiler_rt_builtins": builtins,
            "linker": "lld-link (Chromium package)",
            "libcxx_note":
                "libcxx.lib carries the libc++ runtime objects (the GN source_set that "
                "executables normally get from build/config:common_deps); webrtc.lib "
                "alone has unresolved std::__Cr symbols" if uses_libcxx else None,
        },
    }
    json.dump(out, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
