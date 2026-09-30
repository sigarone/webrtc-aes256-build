#!/usr/bin/env bash
# netem.sh on|off [loss] [reorder]: impairs UDP (and only UDP: signalling stays on TCP) on the loopback
# interface, which carries all browser <-> qjanus media of the same-host test. Needs root.
#   on   : 5 % loss (default), 3 % of the packets overtake the ones delayed by 10 ms (default), IPv4 + IPv6
#   off  : removes it
set -euo pipefail
DEV=lo
case "${1:-}" in
  on)
    LOSS=${2:-5%}
    REORDER=${3:-3%}
    for m in sch_prio sch_netem cls_u32; do modprobe "$m" 2> /dev/null || true; done
    tc qdisc del dev "$DEV" root 2> /dev/null || true
    tc qdisc add dev "$DEV" root handle 1: prio bands 4 priomap 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
    tc qdisc add dev "$DEV" parent 1:4 handle 40: netem delay 10ms reorder "$REORDER" 50% loss "$LOSS"
    tc filter add dev "$DEV" protocol ip parent 1: prio 1 u32 match ip protocol 17 0xff flowid 1:4
    tc filter add dev "$DEV" protocol ipv6 parent 1: prio 2 u32 match ip6 protocol 17 0xff flowid 1:4
    tc qdisc show dev "$DEV"
    ;;
  off)
    tc qdisc del dev "$DEV" root 2> /dev/null || true
    tc qdisc show dev "$DEV"
    ;;
  *) echo "usage: netem.sh on|off [loss] [reorder]" >&2; exit 2 ;;
esac
