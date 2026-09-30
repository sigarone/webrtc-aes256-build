/* Unit test for log-scrub.h: cc -Wall -Wextra -Werror -I. test_scrub.c -o test_scrub && ./test_scrub */
#include <stdio.h>
#include <string.h>
#include "log-scrub.h"

static int failures = 0;
static void check(const char *in, const char *want) {
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", in);
	size_t len = strlen(buf);
	janus_log_scrub(buf);
	if(strlen(buf) != len || strcmp(buf, want) != 0) {
		printf("FAIL: '%s'\n   got '%s'\n  want '%s'\n", in, buf, want);
		failures++;
	}
}

/* A long hex run keeps its first 8 characters, the rest becomes '*', text around it stays */
static void check_run(const char *hex) {
	char in[256], want[256];
	size_t n = strlen(hex);
	snprintf(in, sizeof(in), "Kicked user %s from room x", hex);
	snprintf(want, sizeof(want), "Kicked user %.8s%.*s from room x", hex, (int)(n - 8),
		"****************************************************************************************");
	check(in, want);
}

int main(void) {
	/* identifiers: 12+ hex digits keep 8 characters */
	check_run("a1b2c3d4e5f60718293a4b5c6d7e8f90");
	check_run("A1B2C3D4E5F60718293A4B5C6D7E8F90");
	check_run("00112233445566778899aabbccddeeff");
	check_run("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
	check("id=abcdef012345 x", "id=abcdef01**** x");
	check("abcdef01234 (11)", "abcdef01234 (11)");
	check("deadbeef and cafe", "deadbeef and cafe");
	check("[7143913781946353] Creating new handle", "[71439137********] Creating new handle");
	check("[Tue Sep 30 11:16:12 2026] [123456789] ok", "[Tue Sep 30 11:16:12 2026] [123456789] ok");
	check("user_a1b2c3d4e5f60718", "user_a1b2c3d4********");
	check("token=1793445000,janus,janus.plugin.videoroom", "token=1793445000,janus,janus.plugin.videoroom");
	/* IPv4 */
	check("127.0.0.1", "xxx.x.x.x");
	check("from 192.168.10.20:8188 ok", "from xxx.xxx.xx.xx:8188 ok");
	check("bind 0.0.0.0:8088", "bind x.x.x.x:8088");
	check("(203.0.113.7)", "(xxx.x.xxx.x)");
	check("Janus 1.4.2 libsrtp2 2.8.1 glib 2.80.0 nice 0.1.24", "Janus 1.4.2 libsrtp2 2.8.1 glib 2.80.0 nice 0.1.24");
	check("v1.2.3.4.5 build", "v1.2.3.4.5 build");
	check("1234.5.6.7", "1234.5.6.7");
	check("a1.2.3.4", "a1.2.3.4");
	/* IPv6 */
	check("addr 2a01:4f9:c010:1234::1 up", "addr xxxx:xxx:xxxx:xxxx::x up");
	check("fe80::1:2:3:4%eth0", "xxxx::x:x:x:x%eth0");
	check("::1", "::x");
	check("11:16:12", "11:16:12");
	/* fingerprints */
	check("Fingerprint of our certificate: AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89",
		"Fingerprint of our certificate: xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx");
	/* DTLS-POLICY line stays readable */
	check("[71439137********] DTLS-POLICY version=0xfefc cipher=0x1302 srtp=0x0008 group=0x11ec role=server ok=1 dtls_in=4pkts/2926B dtls_out=3pkts/1799B",
		"[71439137********] DTLS-POLICY version=0xfefc cipher=0x1302 srtp=0x0008 group=0x11ec role=server ok=1 dtls_in=4pkts/2926B dtls_out=3pkts/1799B");
	/* combined and edge cases */
	check("", "");
	check("x", "x");
	check(":::", ":::");
	check("a:b", "a:b");
	if(failures == 0)
		printf("log-scrub: all checks passed\n");
	return failures ? 1 : 0;
}
