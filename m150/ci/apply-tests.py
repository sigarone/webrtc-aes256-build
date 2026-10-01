#!/usr/bin/env python3
"""apply-tests.py - add the Q-Audion unit tests of m150/tests to a patched checkout.

usage: apply-tests.py <webrtc_src> [--dev-target]

The tests are NOT part of the shipped patch series (m150/series): they only
exist in the test jobs, so the patch set that BUILDINFO.json lists and the
binaries stay exactly what the apps get.

  T6   rtc_base/qaudion_tuning_unittest.cc (new file): the runtime tuning API
       and the "Q-AUDION build m150 transport=..." marker of P8.
  T11  m150/tests/ssl_stream_adapter_strict_tests.inc is spliced into
       rtc_base/ssl_stream_adapter_unittest.cc (strict configuration only,
       guarded by QAUDION_TRANSPORT_STRICT inside the include): a stock-like
       peer (DTLS 1.2 only, AES-128 SRTP only, no DTLS-SRTP) must not connect
       to a strict peer, plus positive controls and the strict CryptoOptions.

Both are compiled into rtc_base_unittests, i.e. into rtc_unittests. Every
anchor is checked: a moved anchor fails the job instead of silently skipping
a test. Idempotent (a second run changes nothing).

  --dev-target  also define a small test executable (qaudion_dev_unittests =
                rtc_base_unittests + test main) so the tests can be built and
                run in minutes. Used by the branch-only dev workflow, never by
                the release workflows.
exit: 0 ok | 1 anchor missing / unexpected content | 2 usage
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.join(os.path.dirname(HERE), "tests")
MARK_INC = "// Q-Audion strict-transport tests (T11)."


def die(msg):
    print("::error::apply-tests: " + msg, file=sys.stderr)
    sys.exit(1)


def read(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def block_end(text, start):
    """Index just after the '}' that closes the '{' found at/after start."""
    i = text.index("{", start)
    depth = 0
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    die("unbalanced braces in rtc_base/BUILD.gn")


def insert_after_line(text, anchor_line, new_line, lo, hi):
    """Insert new_line (same indent as the anchor) after the first line equal
    to anchor_line (stripped) found in text[lo:hi]."""
    pos = lo
    while True:
        nl = text.find("\n", pos, hi)
        if nl == -1:
            die("anchor not found: %s" % anchor_line.strip())
        line = text[pos:nl]
        if line.strip() == anchor_line.strip():
            indent = line[: len(line) - len(line.lstrip())]
            return text[: nl + 1] + indent + new_line + "\n" + text[nl + 1:], len(indent + new_line) + 1
        pos = nl + 1


def patch_build_gn(src, dev_target):
    path = os.path.join(src, "rtc_base", "BUILD.gn")
    text = read(path)
    if "qaudion_tuning_unittest.cc" in text:
        print("apply-tests: rtc_base/BUILD.gn already patched")
        return
    start = text.find('rtc_library("rtc_base_unittests") {')
    if start == -1:
        die('rtc_library("rtc_base_unittests") not found in rtc_base/BUILD.gn')
    end = block_end(text, start)
    block = text[start:end]
    s0 = block.find("sources = [")
    d0 = block.find("deps = [")
    if s0 == -1 or d0 == -1 or d0 < s0:
        die("sources/deps lists of rtc_base_unittests not found")
    # sources: right after openssl_stream_adapter_unittest.cc (first list)
    block, grown = insert_after_line(block, '"openssl_stream_adapter_unittest.cc",',
                                     '"qaudion_tuning_unittest.cc",', s0, d0)
    d0 += grown
    # deps: right after :null_socket_server
    block, _ = insert_after_line(block, '":null_socket_server",', '":qaudion_tuning",', d0, len(block))
    text = text[:start] + block + text[end:]
    if dev_target:
        end = start + len(block)
        extra = (
            '\n    # branch-only dev target (apply-tests.py --dev-target)\n'
            '    rtc_test("qaudion_dev_unittests") {\n'
            '      testonly = true\n'
            '      deps = [\n'
            '        ":rtc_base_unittests",\n'
            '        "../test:test_main",\n'
            '      ]\n'
            '    }\n')
        text = text[:end] + extra + text[end:]
    write(path, text)
    print("apply-tests: rtc_base/BUILD.gn patched")


def patch_ssl_test(src):
    path = os.path.join(src, "rtc_base", "ssl_stream_adapter_unittest.cc")
    text = read(path)
    if MARK_INC in text:
        print("apply-tests: ssl_stream_adapter_unittest.cc already patched")
        return
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")
    inc = read(os.path.join(TESTS, "ssl_stream_adapter_strict_tests.inc")).replace("\r\n", "\n")
    if MARK_INC not in inc:
        die("marker missing from ssl_stream_adapter_strict_tests.inc")
    # includes (sorted position)
    a = "#include <openssl/digest.h>\n"
    if a not in text:
        die("anchor '#include <openssl/digest.h>' not found")
    text = text.replace(a, "#include <openssl/bio.h>\n" + a + "#include <openssl/err.h>\n", 1)
    # splice before the two closing namespaces at the end of the file
    tail = "}  // namespace\n}  // namespace webrtc\n"
    idx = text.rfind(tail)
    if idx == -1 or text[idx + len(tail):].strip():
        die("closing namespaces not found at the end of ssl_stream_adapter_unittest.cc")
    text = text[:idx] + inc + ("\n" if not inc.endswith("\n\n") else "") + text[idx:]
    if crlf:
        text = text.replace("\n", "\r\n")
    write(path, text)
    print("apply-tests: ssl_stream_adapter_unittest.cc patched")


def main():
    args = [a for a in sys.argv[1:] if a != "--dev-target"]
    dev = len(args) != len(sys.argv) - 1
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    src = args[0]
    if not os.path.isdir(os.path.join(src, "rtc_base")):
        die("%s is not a webrtc checkout" % src)
    dst = os.path.join(src, "rtc_base", "qaudion_tuning_unittest.cc")
    write(dst, read(os.path.join(TESTS, "qaudion_tuning_unittest.cc")).replace("\r\n", "\n"))
    patch_build_gn(src, dev)
    patch_ssl_test(src)
    return 0


if __name__ == "__main__":
    sys.exit(main())
