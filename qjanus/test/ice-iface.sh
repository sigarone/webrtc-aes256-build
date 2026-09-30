#!/usr/bin/env bash
# ice-iface.sh: unit test of the ICE interface selection (libexec/qjanus-lib.sh): which interfaces
# install.sh and qjanus-render-config accept as QJANUS_ICE_ENFORCE_IFACE. Runs against a fake sysfs and
# a stub `ip`, so it needs no root, no network and no real interfaces (bash + awk + sed only).
#   bash qjanus/test/ice-iface.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/net" "$T/link" "$T/addr" "$T/bin"

# the stub: what `ip` prints for the three calls the library makes (same layout as iproute2's -o output)
cat > "$T/bin/ip" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
d=${FAKE_IP_DIR:?}
args="$*"
case "$args" in
  "-o link show dev "*)  f=$d/link/${args##* }; [ -f "$f" ] && cat "$f" || exit 1 ;;
  "-o addr show dev "*"scope global") n=${args#-o addr show dev }; n=${n%% *}; [ -f "$d/addr/$n" ] && cat "$d/addr/$n" || true ;;
  "-o -4 route get 192.0.2.1") [ -f "$d/route4" ] && cat "$d/route4" || exit 1 ;;
  "-o -6 route get 2001:db8::1") [ -f "$d/route6" ] && cat "$d/route6" || exit 1 ;;
  *) echo "ip stub: unexpected call: $args" >&2; exit 99 ;;
esac
STUB
chmod +x "$T/bin/ip"
export FAKE_IP_DIR=$T PATH="$T/bin:$PATH" QJANUS_SYS_NET=$T/net
unset QJANUS_NAT_1_1 QJANUS_ALLOW_PRIVATE_ICE_IFACE

# iface NAME FLAGS ADDR...   (ADDR: "4:1.2.3.4/24" or "6:2001:db8::5/64")
iface() {
  local n=$1 flags=$2; shift 2
  mkdir -p "$T/net/$n"
  echo "2: $n: <$flags> mtu 1500 qdisc fq_codel state UP mode DEFAULT group default qlen 1000\\    link/ether 52:54:00:12:34:56 brd ff:ff:ff:ff:ff:ff" > "$T/link/$n"
  : > "$T/addr/$n"
  local a
  for a in "$@"; do
    case $a in
      4:*) echo "2: $n    inet ${a#4:} brd 255.255.255.255 scope global $n\\       valid_lft forever preferred_lft forever" >> "$T/addr/$n" ;;
      6:*) echo "2: $n    inet6 ${a#6:} scope global \\       valid_lft forever preferred_lft forever" >> "$T/addr/$n" ;;
    esac
  done
}
UP=BROADCAST,MULTICAST,UP,LOWER_UP

# shellcheck source=../libexec/qjanus-lib.sh
. "$HERE/../libexec/qjanus-lib.sh"

fails=0
pass() { echo "PASS  $*"; }
flunk() { echo "FAIL  $*"; fails=$((fails + 1)); }
# expect_ok NAME [desc]; expect_no NAME PATTERN (the reason that must be printed)
expect_ok() { if err=$(qjanus_check_ice_iface "$1" 2>&1); then pass "$2"; else flunk "$2: refused: $err"; fi; }
expect_no() { if err=$(qjanus_check_ice_iface "$1" 2>&1); then flunk "$3: accepted"; elif [[ $err == *$2* ]]; then pass "$3"; else flunk "$3: wrong reason: $err"; fi; }

iface eth0 "$UP" 4:203.0.113.5/24 6:2001:db8::5/64
iface enx3 "$UP" 4:198.51.100.7/32
iface ens3 "$UP" 4:10.1.2.3/24
iface cg0 "$UP" 4:100.64.1.2/10
iface ula0 "$UP" 6:fd00::1/64
iface v6only "$UP" 6:2a01:4f9::5/64
iface mixed0 "$UP" 4:203.0.113.6/24 4:192.168.5.5/24
iface bare0 "$UP"
iface down0 BROADCAST,MULTICAST 4:203.0.113.8/24
iface nolink0 BROADCAST,MULTICAST,UP 4:203.0.113.9/24
iface loop0 LOOPBACK,UP,LOWER_UP 4:203.0.113.10/24
for n in tailscale0 qvpn0 wg0 docker0 br-abcd veth1234 virbr0 lo vmnet1 lxcbr0; do iface "$n" "$UP" 4:203.0.113.20/24; done

expect_ok eth0 "a public IPv4 + IPv6 interface is accepted"
expect_ok enx3 "a public IPv4-only interface is accepted (Aruba style name)"
expect_ok v6only "a public IPv6-only interface is accepted"
expect_ok mixed0 "public + private addresses on the enforced interface: accepted (its own addresses may be offered)"
for n in tailscale0 qvpn0 wg0 docker0 br-abcd veth1234 virbr0 lo vmnet1 lxcbr0; do
  expect_no "$n" "never offered as ICE candidates" "$n is refused even with a public-looking address"
done
expect_no ens3 "private addresses only" "a private-only interface is refused (RFC 1918)"
expect_no cg0 "private addresses only" "a CGNAT-only interface is refused (100.64/10)"
expect_no ula0 "private addresses only" "a unique-local-only interface is refused"
expect_no bare0 "no global-scope address" "an interface without an address is refused"
expect_no missing0 "does not exist" "a missing interface is refused"
expect_no down0 "not up" "an interface that is down is refused"
expect_no nolink0 "no link" "an interface without carrier is refused"
expect_no loop0 "loopback" "an interface flagged LOOPBACK is refused"
for n in 'eth0;x' 'a b' '1eth' 'eth0123456789abcd' deadbeef ''; do
  expect_no "$n" "not a usable interface name" "the name '$n' is refused"
done

# overrides, for what an operator does on purpose
if QJANUS_NAT_1_1=203.0.113.99 qjanus_check_ice_iface ens3 2> /dev/null; then pass "private-only accepted behind a 1:1 NAT (QJANUS_NAT_1_1)"; else flunk "QJANUS_NAT_1_1 must allow a private-only interface"; fi
if QJANUS_ALLOW_PRIVATE_ICE_IFACE=yes qjanus_check_ice_iface ens3 2> /dev/null; then pass "private-only accepted with QJANUS_ALLOW_PRIVATE_ICE_IFACE=yes"; else flunk "QJANUS_ALLOW_PRIVATE_ICE_IFACE=yes must allow a private-only interface"; fi
if QJANUS_ALLOW_PRIVATE_ICE_IFACE=no qjanus_check_ice_iface ens3 2> /dev/null; then flunk "QJANUS_ALLOW_PRIVATE_ICE_IFACE=no must not allow anything"; else pass "only the value yes overrides"; fi
if QJANUS_ALLOW_PRIVATE_ICE_IFACE=yes qjanus_check_ice_iface tailscale0 2> /dev/null; then flunk "the override must never allow a VPN interface"; else pass "the override never allows a VPN/bridge/loopback interface"; fi

# Janus matches interface names by PREFIX: eth0 must not be used while eth0.100 (or eth01) exists
iface eth0.100 "$UP" 4:203.0.113.30/24
expect_no eth0 "matches interface names by prefix" "an enforced name that is the prefix of another interface is refused"
rm -rf "$T/net/eth0.100"
iface eth1 "$UP" 4:203.0.113.31/24
iface eth10 "$UP" 4:203.0.113.32/24
expect_no eth1 "eth10" "eth1 is refused while eth10 exists"
expect_ok eth10 "eth10 itself is fine (no interface starts with eth10)"
expect_ok eth0 "eth0 is fine again once eth0.100 is gone"

# default-route detection (install.sh names it and asks for confirmation)
echo '192.0.2.1 via 203.0.113.1 dev eth0 src 203.0.113.5 uid 1000 \    cache' > "$T/route4"
[ "$(qjanus_default_iface)" = eth0 ] && pass "the default-route interface is found (IPv4)" || flunk "default interface not found (IPv4)"
rm "$T/route4"; echo '2001:db8::1 from :: via fe80::1 dev v6only src 2a01:4f9::5 metric 1024 pref medium' > "$T/route6"
[ "$(qjanus_default_iface)" = v6only ] && pass "the default-route interface is found (IPv6 fallback)" || flunk "default interface not found (IPv6)"
rm "$T/route6"
[ -z "$(qjanus_default_iface)" ] && pass "no default route: nothing is guessed" || flunk "a default interface was invented"

# the ignore list is what the template renders and what the names above are checked against
[ "$QJANUS_ICE_IGNORE_LIST" = "vmnet,docker,veth,br-,virbr,lxc,wg,tailscale,qvpn,lo" ] && pass "ignore list as specified (wg,docker,veth,br-,tailscale,qvpn,lo,virbr + vmnet,lxc)" || flunk "unexpected ignore list $QJANUS_ICE_IGNORE_LIST"
grep -q '@QJANUS_ICE_IGNORE_LIST@' "$HERE/../conf/janus.jcfg.tmpl" && grep -q '@QJANUS_ICE_ENFORCE_IFACE@' "$HERE/../conf/janus.jcfg.tmpl" && pass "the template carries both lists" || flunk "the template lacks a list placeholder"

echo
if [ "$fails" -ne 0 ]; then echo "ice-iface: $fails FAILED"; exit 1; fi
echo "ice-iface: all checks passed"
