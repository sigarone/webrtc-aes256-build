#!/usr/bin/env python3
"""test-windows-ci.py - tests for the Windows release CI that need no build.

  python3 m150/ci/test-windows-ci.py

Pure Python (standard library only) plus bash for the two workflow steps that
are shell. Run by .github/workflows/test-windows-ci.yml on every pull request
and push to main that touches these files (a few seconds on a hosted runner).

1. link-smoke.py validate_flags: build-flags.json is data produced by an earlier
   job, so every path and token that reaches a command line or os.path.join is
   validated. The real published file must pass; hostile values (an escape
   through "//", "..", "." or an empty segment, a drive letter, a leading
   "-", "+", "/" or "@") must not.
2. build-m150-windows.yml, step "Refuse publish=true from any ref but main"
   (precheck): extracted from the workflow and run with bash against every
   combination of publish and ref.
3. build-m150-windows.yml, step "The release is exactly the 6 expected files plus
   SHA256SUMS" (publish): extracted from the workflow and run with bash against
   synthetic out/ directories.

The two shell tests run the REAL step text taken from the workflow file, so a
step that is removed, renamed or weakened fails here.
"""
import contextlib
import hashlib
import importlib.util
import io
import ntpath
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
WORKFLOW = os.path.join(HERE, "..", "..", ".github", "workflows", "build-m150-windows.yml")


def load_link_smoke():
    spec = importlib.util.spec_from_file_location("link_smoke", os.path.join(HERE, "link-smoke.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


LS = load_link_smoke()


# --------------------------------------------------------------------------
# 1. link-smoke.py validators
# --------------------------------------------------------------------------

def good_flags():
    """The shape and values of the published build-flags.json (release
    webrtc-windows-m150-a256-dplc-10): include dirs, flags, libs exactly as
    released."""
    return {
        "schema": "qaudion-webrtc-buildflags/1",
        "compile": {
            "defines": ["QAUDION_TRANSPORT_STRICT=1", "WEBRTC_WIN", "NOMINMAX", "_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE"],
            "include_dirs": ["../..", "gen", "../../buildtools/third_party/libc++",
                             "../../third_party/abseil-cpp", "../../third_party/perfetto/include",
                             "gen/third_party/perfetto/build_config", "gen/third_party/perfetto",
                             "../../third_party/libyuv/include",
                             "../../third_party/libc++/src/include",
                             "../../third_party/boringssl/src/include"],
            "flags": ["-fcomplete-member-pointers", "/utf-8", "/Zc:twoPhase", "-fmsc-version=1934",
                      "-m64", "-msse3", "/Zc:inline", "/MT", "/std:c++20"],
            "header_root": "include",
        },
        "link": {
            "compiler_rt_builtins": "lib/clang/23/lib/windows/clang_rt.builtins-x86_64.lib",
            "default_libs": ["libcpmt.lib"],
            "static_libs": ["webrtc.lib", "libcxx.lib"],
            "system_libs": ["crypt32.lib", "iphlpapi.lib", "secur32.lib", "winmm.lib", "dmoguids.lib",
                            "wmcodecdspuuid.lib", "amstrmid.lib", "msdmo.lib", "oleaut32.lib",
                            "ole32.lib", "strmiids.lib", "user32.lib", "bcrypt.lib", "d3d11.lib",
                            "dxgi.lib", "shcore.lib", "dwmapi.lib"],
        },
        "abi": {"stl": "libc++"},
        "toolchain": {
            "clang_package_url": "https://commondatastorage.googleapis.com/chromium-browser-clang/Win/clang-llvmorg-23-init-1234-gabcdef01-1.tar.xz",
            "clang_package_sha256": "0" * 64,
        },
    }


def validate(flags):
    """validate_flags(flags) -> None when accepted, the die() message when not."""
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        try:
            LS.validate_flags(flags)
        except SystemExit as e:
            assert e.code == 1, "die() must exit 1, got %r" % (e.code,)
            return err.getvalue()
    return None


# Hostile or malformed include dirs. Every one was accepted, or could be
# turned into an absolute path by os.path.join on Windows, before the
# segment-based validation (the first one is the finding itself).
HOSTILE_INCLUDE_DIRS = [
    "../..//Windows/System32",        # empty segment: join() restarts at an absolute path
    "../..//",
    "../../",                         # trailing empty segment
    "../../a/",
    "../../a//b",
    "../../.",                        # "." segment
    "../../a/./b",
    "../../..",                       # a third ".."
    "../../../etc",
    "../../a/../..",                  # ".." after the leading pair
    "../../a/..",
    "../..\\Windows",                 # backslash
    "../../a\\b",
    "../../C:/Windows",               # drive letter / NTFS stream
    "../../C:",
    "../../a:b",
    "../../-x",                       # leading "-" / "+" / "@"
    "../../+x",
    "../../@rsp",
    "../../a/-x",
    "../../a b",                      # space
    "../../a\"b",                     # quote (the path ends up inside -I"...")
    "../..\n",                        # newline (a regex $ would let it through)
    "../../a\nb",
    "..",
    "../",
    "../x",
    ".",
    "./gen",
    "gen/",
    "gen//x",
    "gen/../x",
    "gen/..",
    "gen/./x",
    "gen/x/",
    "gen\\x",
    "gen/C:/x",
    "gen/-x",
    "/",
    "/abs",
    "//host/share",
    "/../..",
    "C:/x",
    "C:\\x",
    "@file",
    "-I/x",
    "",
    "other/dir",
    None,
    7,
]

HOSTILE_LIBS = [
    "-foo.lib",                       # "-" / "+" lead: a linker option, not a file
    "+foo.lib",
    "/FORCE.lib",
    "/LIBPATH:C:/x.lib",
    "@x.lib",
    ".lib",
    "..lib",
    ".foo.lib",
    "foo",                            # not a .lib
    "foo.lib\n",
    "foo.lib ",
    " foo.lib",
    "a/b.lib",                        # a path, not a file name
    "../b.lib",
    "/abs.lib",
    "//host/b.lib",
    "C:/b.lib",
    "C:b.lib",
    "a\\b.lib",
    "a b.lib",
    "",
    None,
    7,
]

# header_root and link.compiler_rt_builtins go through safe_rel
HOSTILE_REL_PATHS = [
    "..", "../x", "x/..", "x/../y", ".", "./x", "x/.", "x/./y", "x//y", "x/", "/x", "//x", "//", "",
    "C:/x", "C:x", "x\\y", "-x", "+x", "@x", "x/-y", " x", "x ", "x\n", "x\ny", None, 7,
]


class LinkSmokeValidation(unittest.TestCase):
    def test_published_flags_are_accepted(self):
        self.assertIsNone(validate(good_flags()))

    def test_real_published_file_is_accepted(self):
        """Optional: BUILD_FLAGS_JSON=<path of a downloaded build-flags.json>."""
        path = os.environ.get("BUILD_FLAGS_JSON")
        if not path:
            self.skipTest("BUILD_FLAGS_JSON not set")
        import json
        with open(path, encoding="utf-8") as f:
            self.assertIsNone(validate(json.load(f)))

    def test_include_dir_predicate(self):
        for d in good_flags()["compile"]["include_dirs"]:
            self.assertTrue(LS.is_include_dir(d), d)
        for d in HOSTILE_INCLUDE_DIRS:
            self.assertFalse(LS.is_include_dir(d), repr(d))

    def test_lib_regex(self):
        for l in ("webrtc.lib", "libcxx.lib", "kernel32.lib", "Advapi32.lib", "libc++.lib", "a-b_c.d+e.lib", "_x.lib", "1.lib"):
            self.assertTrue(LS.LIB_RE.fullmatch(l), l)
        for l in HOSTILE_LIBS:
            self.assertFalse(isinstance(l, str) and LS.LIB_RE.fullmatch(l), repr(l))

    def test_rel_path_predicate(self):
        for p in ("include", "lib/clang/23/lib/windows/clang_rt.builtins-x86_64.lib", "a/b-c/d.e+f", "x"):
            self.assertTrue(LS.is_plain_rel_path(p), p)
        for p in HOSTILE_REL_PATHS:
            self.assertFalse(LS.is_plain_rel_path(p), repr(p))

    def test_validate_flags_rejects_each_hostile_include_dir(self):
        for d in HOSTILE_INCLUDE_DIRS:
            f = good_flags()
            f["compile"]["include_dirs"].append(d)
            msg = validate(f)
            self.assertIsNotNone(msg, "include dir accepted: %r" % (d,))
            self.assertIn("include dir", msg)

    def test_validate_flags_rejects_each_hostile_lib(self):
        for l in HOSTILE_LIBS:
            for key in ("system_libs", "static_libs", "default_libs"):
                f = good_flags()
                f["link"][key].append(l)
                msg = validate(f)
                self.assertIsNotNone(msg, "%s accepted: %r" % (key, l))
                self.assertIn("lib", msg)

    def test_validate_flags_rejects_each_hostile_relative_path(self):
        for p in HOSTILE_REL_PATHS:
            f = good_flags()
            f["compile"]["header_root"] = p
            self.assertIsNotNone(validate(f), "header_root accepted: %r" % (p,))
            # the same hostile path as the directory part of the builtins library
            f = good_flags()
            f["link"]["compiler_rt_builtins"] = (p + "/clang_rt.builtins-x86_64.lib") if isinstance(p, str) else p
            self.assertIsNotNone(validate(f), "compiler_rt_builtins accepted: %r" % (f["link"]["compiler_rt_builtins"],))

    def test_the_finding_itself(self):
        """../..//Windows/System32 as an include dir, named explicitly."""
        f = good_flags()
        f["compile"]["include_dirs"] = ["../..//Windows/System32"]
        self.assertIn("include dir", validate(f) or "")

    def test_accepted_dirs_stay_inside_the_header_root(self):
        """main() joins d[len('../../'):] to the unpacked header root. For every
        accepted include dir that join must stay inside the root on Windows
        (ntpath) and POSIX, whatever the host running the test."""
        for mod, root in ((ntpath, "C:\\smoke\\hdr\\include"), (posixpath, "/smoke/hdr/include")):
            for d in good_flags()["compile"]["include_dirs"]:
                if not d.startswith("../../"):
                    continue
                p = mod.normpath(mod.join(root, d[len("../../"):]))
                self.assertTrue(p == root or p.startswith(root + mod.sep), (mod.__name__, d, p))
        # and the hostile one escapes, which is why it must be rejected before the join
        p = ntpath.normpath(ntpath.join("C:\\smoke\\hdr\\include", "../..//Windows/System32"[len("../../"):]))
        self.assertFalse(p.startswith("C:\\smoke\\hdr\\include"), p)

    def test_clang_package_still_validated(self):
        f = good_flags()
        f["toolchain"]["clang_package_sha256"] = "0" * 63
        self.assertIsNotNone(validate(f))
        f = good_flags()
        f["toolchain"]["clang_package_url"] = good_flags()["toolchain"]["clang_package_url"] + "\nhttps://evil.example/x"
        self.assertIsNotNone(validate(f))


# --------------------------------------------------------------------------
# helpers to run a real workflow step with bash
# --------------------------------------------------------------------------

def read_workflow():
    with open(WORKFLOW, encoding="utf-8", newline="") as f:
        return f.read().replace("\r\n", "\n")


def workflow_steps(text):
    """[(job, step_name, run_script_or_None)] for every step of every job. The
    workflow is regular enough (jobs at 2 spaces, steps at 6, run: | at 8) for
    a line scan; no YAML library needed."""
    out = []
    job = None
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        l = lines[i]
        m = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", l)
        if m and not l.startswith("   "):
            job = m.group(1)
        m = re.match(r"^      - name: (.*)$", l)
        if m:
            name = m.group(1).strip()
            j = i + 1
            run = None
            while j < len(lines) and not re.match(r"^      - ", lines[j]) and (not lines[j].strip() or lines[j].startswith("      ")):
                if lines[j] == "        run: |":
                    body = []
                    k = j + 1
                    while k < len(lines) and (not lines[k].strip() or lines[k].startswith("          ")):
                        body.append(lines[k][10:] if lines[k].strip() else "")
                        k += 1
                    run = "\n".join(body).rstrip("\n") + "\n"
                    j = k
                    continue
                j += 1
            out.append((job, name, run))
            i = j
            continue
        i += 1
    return out


def find_step(job, name_prefix):
    text = read_workflow()
    hits = [(j, n, r) for (j, n, r) in workflow_steps(text) if j == job and n.startswith(name_prefix)]
    if len(hits) != 1:
        raise AssertionError("expected exactly one step %r in job %r of the workflow, found %d" % (name_prefix, job, len(hits)))
    if hits[0][2] is None:
        raise AssertionError("step %r has no run: | block" % name_prefix)
    return hits[0][2]


def find_bash():
    b = shutil.which("bash")
    if not b:
        if os.environ.get("CI"):
            raise AssertionError("bash not found in CI")
        raise unittest.SkipTest("bash not found")
    return b


def run_bash(script, cwd, env_extra):
    """Run the script text exactly as the Actions runner does: bash -e file.sh
    (the step itself sets -euo pipefail)."""
    bash = find_bash()
    env = {k: v for k, v in os.environ.items() if k not in ("IN_PUBLISH", "RUN_REF", "GITHUB_OUTPUT")}
    env.update(env_extra)
    with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False, newline="\n", encoding="utf-8") as f:
        f.write(script)
        path = f.name
    try:
        return subprocess.run([bash, path], cwd=cwd, env=env, capture_output=True, text=True, timeout=120)
    finally:
        os.unlink(path)


# --------------------------------------------------------------------------
# 2. precheck: publish=true only from main
# --------------------------------------------------------------------------

PRECHECK_STEP = "Refuse publish=true from any ref but main"


class PublishOnlyFromMain(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = find_step("precheck", PRECHECK_STEP)

    def run_step(self, publish, ref):
        env = {"RUN_REF": ref}
        if publish is not None:
            env["IN_PUBLISH"] = publish
        with tempfile.TemporaryDirectory() as d:
            r = run_bash(self.script, d, env)
            self.assertEqual(os.listdir(d), [], "the step must not create files (injection?)")
            return r

    def test_publish_true_from_main_passes(self):
        r = self.run_step("true", "refs/heads/main")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_publish_true_from_a_branch_fails(self):
        for ref in ("refs/heads/fix/s0-review-hardening", "refs/heads/feature", "refs/heads/main-2",
                    "refs/heads/mainx", "refs/heads/main/x", "refs/heads/Main", "refs/tags/v1",
                    "refs/pull/13/merge", "main", "", "refs/heads/main ",
                    "refs/heads/$(touch pwned)", "refs/heads/`touch pwned`", "refs/heads/x;touch pwned"):
            r = self.run_step("true", ref)
            self.assertNotEqual(r.returncode, 0, "publish=true accepted from %r" % (ref,))
            self.assertIn("::error::", r.stdout + r.stderr)

    def test_publish_false_passes_from_anywhere(self):
        for ref in ("refs/heads/main", "refs/heads/fix/s0-review-hardening", "refs/tags/v1"):
            r = self.run_step("false", ref)
            self.assertEqual(r.returncode, 0, "publish=false refused on %r: %s" % (ref, r.stdout + r.stderr))

    def test_publish_unset_passes(self):
        """A run that carries no publish input never publishes (the publish job
        needs inputs.publish == 'true'), so there is nothing to refuse."""
        r = self.run_step(None, "refs/heads/fix/x")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = self.run_step("", "refs/heads/fix/x")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_publish_must_be_a_boolean(self):
        for v in ("TRUE", "True", "yes", "1", "false ", " true", "true\nfalse", "$(touch pwned)"):
            r = self.run_step(v, "refs/heads/main")
            self.assertNotEqual(r.returncode, 0, "publish=%r accepted" % (v,))

    def test_it_is_wired_to_the_right_inputs(self):
        text = read_workflow()
        self.assertIn("  IN_PUBLISH: ${{ github.event.inputs.publish }}\n", text)
        self.assertIn("  RUN_REF: ${{ github.ref }}\n", text)
        # it is the FIRST step of precheck: it fails in seconds, before anything else
        steps = [(j, n) for (j, n, _r) in workflow_steps(text) if j == "precheck"]
        self.assertTrue(steps and steps[0][1].startswith(PRECHECK_STEP), steps)

    def test_publish_job_still_requires_main(self):
        text = read_workflow()
        m = re.search(r"^    if: (.*needs\.build\.result.*)$", text, re.M)
        self.assertTrue(m)
        self.assertIn("github.ref == 'refs/heads/main'", m.group(1))
        self.assertIn("github.event.inputs.publish == 'true'", m.group(1))

    def test_no_expression_is_interpolated_into_a_run_block(self):
        """The workflow's own rule: inputs and contexts reach the shell only via env."""
        for (job, name, run) in workflow_steps(read_workflow()):
            if run is not None:
                self.assertNotIn("${{", run, "step %r (job %s) interpolates an expression into run:" % (name, job))


# --------------------------------------------------------------------------
# 3. publish: the release is exactly the 6 files plus SHA256SUMS
# --------------------------------------------------------------------------

FILESET_STEP = "The release is exactly the 6 expected files plus SHA256SUMS"
EXPECTED = ["webrtc.lib", "libcxx.lib", "webrtc-headers.zip", "build-flags.json", "BUILDINFO.json", "PINS.txt"]


def make_out(d, files=None, listed=None, binary=True):
    """out/ with the given files (default: the 6) and a SHA256SUMS of real
    hashes for `listed` (default: the files)."""
    files = EXPECTED if files is None else files
    listed = files if listed is None else listed
    for n in files:
        with open(os.path.join(d, n), "wb") as f:
            f.write(("content of " + n).encode())
    lines = []
    for n in listed:
        data = ("content of " + n).encode()
        lines.append("%s %s%s\n" % (hashlib.sha256(data).hexdigest(), "*" if binary else " ", n))
    with open(os.path.join(d, "SHA256SUMS"), "w", newline="\n") as f:
        f.write("".join(lines))


class ReleaseFileSet(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = find_step("publish", FILESET_STEP)

    def check(self, **kw):
        extra = kw.pop("extra", None)
        with tempfile.TemporaryDirectory() as d:
            make_out(d, **kw)
            if extra:
                extra(d)
            return run_bash(self.script, d, {})

    def test_exactly_the_expected_set_passes(self):
        r = self.check()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_text_mode_checksum_lines_pass(self):
        r = self.check(binary=False)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_a_missing_file_fails(self):
        for gone in EXPECTED:
            rest = [n for n in EXPECTED if n != gone]
            # missing and still listed
            self.assertNotEqual(self.check(files=rest, listed=EXPECTED).returncode, 0, "missing+listed " + gone)
            # missing and not listed either: consistent with each other, but not the fixed set
            self.assertNotEqual(self.check(files=rest, listed=rest).returncode, 0, "missing+unlisted " + gone)

    def test_an_extra_file_fails(self):
        # present, not listed
        self.assertNotEqual(self.check(files=EXPECTED + ["extra.bin"], listed=EXPECTED).returncode, 0)
        # present AND listed: out/ and SHA256SUMS agree, the set is still not the fixed one
        r = self.check(files=EXPECTED + ["extra.bin"], listed=EXPECTED + ["extra.bin"])
        self.assertNotEqual(r.returncode, 0, "an extra, checksummed file rode along")
        self.assertIn("::error::", r.stdout + r.stderr)

    def test_a_hidden_file_fails(self):
        self.assertNotEqual(self.check(files=EXPECTED + [".hidden"]).returncode, 0)

    def test_checksums_listing_a_missing_or_extra_file_fails(self):
        self.assertNotEqual(self.check(listed=EXPECTED + ["ghost.bin"]).returncode, 0)
        self.assertNotEqual(self.check(listed=EXPECTED[:-1]).returncode, 0)
        self.assertNotEqual(self.check(listed=EXPECTED + ["SHA256SUMS"]).returncode, 0)

    def test_a_file_listed_twice_fails(self):
        self.assertNotEqual(self.check(listed=EXPECTED + ["PINS.txt"]).returncode, 0)

    def test_a_directory_instead_of_a_file_fails(self):
        def swap(d):
            os.unlink(os.path.join(d, "PINS.txt"))
            os.mkdir(os.path.join(d, "PINS.txt"))
        self.assertNotEqual(self.check(extra=swap).returncode, 0)

    def test_a_subdirectory_fails(self):
        self.assertNotEqual(self.check(extra=lambda d: os.mkdir(os.path.join(d, "sub"))).returncode, 0)

    def test_a_symlink_instead_of_a_file_fails(self):
        def link(d):
            os.unlink(os.path.join(d, "PINS.txt"))
            try:
                os.symlink(os.path.join(d, "BUILDINFO.json"), os.path.join(d, "PINS.txt"))
            except (OSError, NotImplementedError):
                raise unittest.SkipTest("cannot create symlinks here")
        self.assertNotEqual(self.check(extra=link).returncode, 0)

    def test_malformed_checksum_lines_fail(self):
        def mangle(d):
            p = os.path.join(d, "SHA256SUMS")
            with open(p, newline="") as f:
                s = f.read()
            with open(p, "w", newline="") as f:
                f.write(s.replace("*PINS.txt", "*PINS.txt extra"))
        self.assertNotEqual(self.check(extra=mangle).returncode, 0)

    def test_the_step_is_in_the_publish_job_after_the_checksum_check(self):
        names = [n for (j, n, _r) in workflow_steps(read_workflow()) if j == "publish"]
        i = [k for k, n in enumerate(names) if n.startswith("Re-verify SHA256SUMS")]
        j = [k for k, n in enumerate(names) if n.startswith(FILESET_STEP)]
        self.assertTrue(i and j and i[0] < j[0], names)


if __name__ == "__main__":
    unittest.main(verbosity=2)
