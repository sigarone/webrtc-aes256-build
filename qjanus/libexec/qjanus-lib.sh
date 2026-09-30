#!/usr/bin/env bash
# qjanus-lib.sh: helpers shared by install.sh and qjanus-render-config. Sourced, never executed.

# qjanus_private_addr ADDR: true for the addresses that cannot be reached from the internet:
# IPv4 loopback, RFC 1918, CGNAT 100.64.0.0/10 (what VPNs hand out), IPv4 link-local, IPv6 ::1,
# unique-local fc00::/7 and link-local fe80::/10. Everything else (0.0.0.0 and :: included) is not.
qjanus_private_addr() {
  local a=${1,,}
  if [[ $a =~ ^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})$ ]]; then
    local o1=$((10#${BASH_REMATCH[1]})) o2=$((10#${BASH_REMATCH[2]})) o3=$((10#${BASH_REMATCH[3]})) o4=$((10#${BASH_REMATCH[4]}))
    { [ "$o1" -le 255 ] && [ "$o2" -le 255 ] && [ "$o3" -le 255 ] && [ "$o4" -le 255 ]; } || return 1
    case $o1 in
      127|10) return 0 ;;
      172) { [ "$o2" -ge 16 ] && [ "$o2" -le 31 ]; }; return ;;
      192) [ "$o2" -eq 168 ]; return ;;
      100) { [ "$o2" -ge 64 ] && [ "$o2" -le 127 ]; }; return ;;
      169) [ "$o2" -eq 254 ]; return ;;
    esac
    return 1
  fi
  [[ $a == ::1 ]] && return 0
  [[ $a =~ ^f[cd][0-9a-f]{0,2}: ]] && return 0
  [[ $a =~ ^fe[89ab][0-9a-f]?: ]] && return 0
  return 1
}

# qjanus_check_http_bind ADDR: the server-facing API (port 8088, no TLS) is for the application
# server only. ADDR must look like an address and be a private one, unless the operator sets
# QJANUS_ALLOW_NONPRIVATE_BIND=yes (in /etc/qjanus/qjanus.env) on purpose.
qjanus_check_http_bind() {
  local a=$1
  [[ $a =~ ^[0-9A-Fa-f:.]{2,45}$ ]] || { echo "the HTTP bind address must be an IPv4 or IPv6 address" >&2; return 1; }
  if qjanus_private_addr "$a"; then
    return 0
  fi
  if [ "${QJANUS_ALLOW_NONPRIVATE_BIND:-}" = yes ]; then
    echo "warning: the server API is bound to a non-private address (QJANUS_ALLOW_NONPRIVATE_BIND=yes)" >&2
    return 0
  fi
  echo "refusing the HTTP bind address $a: the server API (port 8088, plain HTTP) must be reachable from the application server only" >&2
  echo "(loopback, private, CGNAT/VPN, unique-local or link-local addresses are accepted; QJANUS_ALLOW_NONPRIVATE_BIND=yes overrides)" >&2
  return 1
}

# ---- the ICE interface
# Janus advertises the address of every interface it finds as an ICE candidate, VPN, bridge and
# container interfaces included: a private address of tailscale0/qvpn0/... would be offered to every
# peer (privacy, and a candidate that can never work). qjanus therefore names ONE interface, the one
# that carries the public address (QJANUS_ICE_ENFORCE_IFACE), rendered as ice_enforce_list; IPv4 and
# IPv6 of that interface are offered, nothing else.
# Janus v1.4.2 matches the name by PREFIX (an enforced "eth0" also takes "eth0.100") and, while an
# enforce list is set, never looks at the ignore list for interface names (it only discards candidate
# addresses that start with an entry): the ignore list below is belt and braces for the day the
# enforce list is empty, and the checks here are what makes the enforce list safe to rely on.
QJANUS_ICE_IGNORE_LIST="vmnet,docker,veth,br-,virbr,lxc,wg,tailscale,qvpn,lo"
QJANUS_SYS_NET=${QJANUS_SYS_NET:-/sys/class/net}

# qjanus_default_iface: the interface the default route of this host uses (IPv4 first), or nothing
qjanus_default_iface() {
  local dev
  dev=$(ip -o -4 route get 192.0.2.1 2> /dev/null | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }') || true
  [ -n "$dev" ] || dev=$(ip -o -6 route get 2001:db8::1 2> /dev/null | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }') || true
  echo "$dev"
}

# qjanus_iface_addrs IFACE: the global-scope addresses of IFACE, one per line, without prefix length
qjanus_iface_addrs() {
  ip -o addr show dev "$1" scope global 2> /dev/null | awk '{ split($4, a, "/"); print a[1] }'
}

# qjanus_check_ice_iface IFACE: true if IFACE may be the ICE interface of this node. Every refusal is
# explained on stderr. Private-only interfaces (a node whose public address is not on any interface)
# are refused, unless QJANUS_NAT_1_1 names the public address that is advertised instead or the
# operator allows it on purpose (QJANUS_ALLOW_PRIVATE_ICE_IFACE=yes).
qjanus_check_ice_iface() {
  local i=$1 p other flags addrs a public=0 n=0
  local -a ignored
  # a name Janus can match as a prefix of ONE interface: letters first (never a prefix of an address),
  # at least one non-hex letter (never a prefix of an IPv6 address), what the kernel allows in a name
  [[ $i =~ ^[A-Za-z][A-Za-z0-9._@-]{0,14}$ ]] || { echo "'$i' is not a usable interface name" >&2; return 1; }
  [[ $i =~ [g-zG-Z] ]] || { echo "'$i' is not a usable interface name (it could be taken for the start of an address)" >&2; return 1; }
  IFS=, read -r -a ignored <<< "$QJANUS_ICE_IGNORE_LIST"
  for p in "${ignored[@]}"; do
    if [[ $i == "$p"* ]]; then
      echo "interface $i is a loopback/VPN/bridge/container interface (name starts with '$p'): its addresses are never offered as ICE candidates" >&2
      return 1
    fi
  done
  [ -d "$QJANUS_SYS_NET/$i" ] || { echo "interface $i does not exist on this host" >&2; return 1; }
  flags=$(ip -o link show dev "$i" 2> /dev/null | sed -n 's/^[^<]*<\([^>]*\)>.*/\1/p')
  case ",$flags," in *,LOOPBACK,*) echo "interface $i is a loopback interface" >&2; return 1 ;; esac
  case ",$flags," in *,UP,*) ;; *) echo "interface $i is not up" >&2; return 1 ;; esac
  case ",$flags," in *,LOWER_UP,*) ;; *) echo "interface $i has no link (no carrier)" >&2; return 1 ;; esac
  # Janus compares the PREFIX: no other interface may start with this name (eth0 would also take eth0.100)
  for other in "$QJANUS_SYS_NET"/*; do
    other=${other##*/}
    if [ "$other" != "$i" ] && [[ $other == "$i"* ]]; then
      echo "interface $other also starts with '$i': Janus matches interface names by prefix, so both would be used" >&2
      return 1
    fi
  done
  addrs=$(qjanus_iface_addrs "$i")
  while read -r a; do
    [ -n "$a" ] || continue
    n=$((n + 1))
    qjanus_private_addr "$a" || public=1
  done <<< "$addrs"
  [ "$n" -gt 0 ] || { echo "interface $i carries no global-scope address" >&2; return 1; }
  if [ "$public" = 0 ]; then
    if [ -n "${QJANUS_NAT_1_1:-}" ] || [ "${QJANUS_ALLOW_PRIVATE_ICE_IFACE:-}" = yes ]; then
      echo "note: interface $i carries private addresses only (accepted: QJANUS_NAT_1_1 or QJANUS_ALLOW_PRIVATE_ICE_IFACE is set)" >&2
    else
      echo "interface $i carries private addresses only: peers on the internet could not reach them (a node behind a 1:1 NAT sets QJANUS_NAT_1_1, --allow-private-ice-iface overrides)" >&2
      return 1
    fi
  fi
  return 0
}
