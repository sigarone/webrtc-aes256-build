#!/usr/bin/env python3
"""apply-tests.py - add the Q-Audion unit tests (T6, T10) to a patched checkout.

usage: apply-tests.py <webrtc_src> [--dev-target]

The tests are NOT part of the patch series (m150/series): the series is what
ships in the libraries, the tests only run in CI. They are added to the
checkout AFTER apply-series.sh and BEFORE `gn gen`:

  * m150/tests/qaudion_tuning_unittest.cc is copied to rtc_base/ and listed in
    rtc_base_unittests (BUILD.gn)                                      -> T6
  * m150/tests/ssl_stream_adapter_strict_tests.inc is spliced into
    rtc_base/ssl_stream_adapter_unittest.cc (inside its anonymous namespace,
    so it uses that file's DTLS fixtures)                             -> T10

Every edit is anchored and verified; an anchor that is not found is an error
(a silent no-op would leave the required tests out of the run). Running it
twice on the same tree is also an error.

--dev-target  additionally defines //rtc_base:qaudion_dev_unittests (an
              executable made of rtc_base_unittests only) so a dev workflow can
              build and run just these tests in minutes. Never used by the
              release workflows.

exit: 0 ok | 1 anchor missing / already applied | 2 usage
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.join(os.path.dirname(HERE), "tests")


def die(msg):
    print("::error::apply-tests: " + msg, file=sys.stderr)
    sys.exit(1)


def read(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def insert_after(text, anchor, new_line, start=0, end=None):
    """Insert new_line (with the indentation of anchor's line) after the first
    line equal to anchor inside text[start:end]."""
    end = len(text) if end is None else end
    i = text.find(anchor, start, end)
    if i < 0:
        die("anchor not found: %r" % anchor.strip())
    line_start = text.rfind("\n", 0, i) + 1
    indent = text[line_start:i]
    eol = text.find("\n", i)
    if eol < 0:
        die("anchor at end of file: %r" % anchor.strip())
    return text[:eol + 1] + indent + new_line + "\n" + text[eol + 1:]


def block_bounds(text, header):
    """[start, end) of the brace block that opens right after `header`."""
    h = text.find(header)
    if h < 0:
        die("block not found: %r" % header)
    open_i = text.find("{", h)
    depth = 0
    for j in range(open_i, len(text)):
        c = text[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return open_i, j + 1
    die("unbalanced block: %r" % header)


def main():
    args = [a for a in sys.argv[1:] if a != "--dev-target"]
    dev = "--dev-target" in sys.argv
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    src = args[0]

    # --- T6: new test file + BUILD.gn wiring -------------------------------
    dst = os.path.join(src, "rtc_base", "qaudion_tuning_unittest.cc")
    if os.path.exists(dst):
        die("already applied (rtc_base/qaudion_tuning_unittest.cc exists)")
    shutil.copyfile(os.path.join(TESTS, "qaudion_tuning_unittest.cc"), dst)

    gn_path = os.path.join(src, "rtc_base", "BUILD.gn")
    gn = read(gn_path)
    b0, b1 = block_bounds(gn, 'rtc_library("rtc_base_unittests")')
    gn = insert_after(gn, '"openssl_stream_adapter_unittest.cc",',
                      '"qaudion_tuning_unittest.cc",', b0, b1)
    # the block moved by one inserted line; recompute
    b0, b1 = block_bounds(gn, 'rtc_library("rtc_base_unittests")')
    gn = insert_after(gn, '":null_socket_server",', '":qaudion_tuning",', b0, b1)
    if dev:
        gn = gn.rstrip("\n") + (
            "\n\nif (rtc_include_tests && !build_with_chromium) {\n"
            "  rtc_test(\"qaudion_dev_unittests\") {\n"
            "    testonly = true\n"
            "    deps = [ \":rtc_base_unittests\" ]\n"
            "  }\n"
            "}\n")
    write(gn_path, gn)

    # --- T10: spliced into the existing SSL stream adapter test file -------
    ut_path = os.path.join(src, "rtc_base", "ssl_stream_adapter_unittest.cc")
    ut = read(ut_path)
    if "QaudionStrictDtlsTest" in ut:
        die("already applied (ssl_stream_adapter_unittest.cc)")
    ut = insert_after(ut, "#include <openssl/digest.h>", "#include <openssl/bio.h>")
    ut = insert_after(ut, "#include <openssl/evp.h>  // IWYU pragma: keep",
                      "#include <openssl/err.h>")
    ut = insert_after(ut, '#include "rtc_base/message_digest.h"',
                      '#include "rtc_base/qaudion_tuning.h"')
    tail = "}  // namespace\n}  // namespace webrtc"
    k = ut.rfind(tail)
    if k < 0:
        die("end-of-file namespace anchor not found in ssl_stream_adapter_unittest.cc")
    inc = read(os.path.join(TESTS, "ssl_stream_adapter_strict_tests.inc"))
    ut = ut[:k] + inc.rstrip("\n") + "\n\n" + ut[k:]
    write(ut_path, ut)
    print("apply-tests: T6 (rtc_base/qaudion_tuning_unittest.cc) and T10 "
          "(ssl_stream_adapter_unittest.cc) added%s" % (" + dev target" if dev else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
