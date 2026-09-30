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
