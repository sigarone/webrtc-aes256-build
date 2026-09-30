#!/usr/bin/env bash
# qjanus node installer. Run as root from the unpacked release directory (the directory that
# holds this script, bin/, lib/ and share/). Idempotent: running it again keeps the DTLS key,
# the secrets and the node settings, and only changes what differs.
#
#   ./install.sh [--http-bind ADDR] [--install-deps] [--no-start] [--keep-releases N]
#   ./install.sh --rollback          switch back to the previous release and restart
#   ./install.sh --uninstall [--purge]
#
# stdout carries ONE line, the pinned DTLS fingerprint ("sha-256 AB:CD:..."), everything else goes
# to stderr, so that   FP=$(sudo ./install.sh ...)   just works. The DTLS private key is generated
# HERE, on the node, and never leaves it.
#
# An upgrade that does not come up healthy is undone by the script itself: the previous release
# (with the unit it shipped) and the previous /etc/qjanus/qjanus.env are put back and started
# again, and the script exits with an error and prints no fingerprint.
set -euo pipefail

SELF=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
OPT=/opt/qjanus
ETC=/etc/qjanus
UNIT=/etc/systemd/system/qjanus.service
ENVF=$ETC/qjanus.env
LOCKF=/run/lock/qjanus-install.lock
KEEP=3
HTTP_BIND=""
INSTALL_DEPS=0
START=1
MODE=install
PURGE=0
DTLS_DAYS=3650
ENV_BACKUP=""

log() { echo "qjanus-install: $*" >&2; }
die() { echo "qjanus-install: ERROR: $*" >&2; exit 1; }
cleanup() { [ -z "$ENV_BACKUP" ] || rm -f "$ENV_BACKUP"; }
trap cleanup EXIT

# shellcheck source=libexec/qjanus-lib.sh
. "$SELF/libexec/qjanus-lib.sh"

fingerprint() {
  local fp
  fp=$(openssl x509 -in "$ETC/dtls.crt" -noout -fingerprint -sha256) || die "cannot read $ETC/dtls.crt"
  echo "sha-256 ${fp#*=}"
}

# set_env KEY VALUE: replaces or appends KEY=VALUE in the env file (0600, root)
set_env() {
  local key=$1 val=$2
  if grep -q "^$key=" "$ENVF"; then
    sed -i "s|^$key=.*|$key=$val|" "$ENVF"
  else
    echo "$key=$val" >> "$ENVF"
  fi
}
get_env() { grep "^$1=" "$ENVF" 2>/dev/null | head -1 | cut -d= -f2- || true; }

while [ $# -gt 0 ]; do
  case "$1" in
    --http-bind) [ $# -ge 2 ] || die "--http-bind needs an address"; HTTP_BIND=$2; shift 2 ;;
    --install-deps) INSTALL_DEPS=1; shift ;;
    --no-start) START=0; shift ;;
    --keep-releases) [ $# -ge 2 ] || die "--keep-releases needs a number"; KEEP=$2; shift 2 ;;
    --rollback) MODE=rollback; shift ;;
    --uninstall) MODE=uninstall; shift ;;
    --purge) PURGE=1; shift ;;
    -h|--help) sed -n '2,/^set -euo pipefail/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//' >&2; exit 0 ;;
    *) die "unknown option $1" ;;
  esac
done
[ "$(id -u)" = 0 ] || die "run as root"
[[ $KEEP =~ ^[0-9]+$ ]] && [ "$KEEP" -ge 2 ] || die "--keep-releases must be a number >= 2"
# the server API is plain HTTP for the application server only: never a public address
if [ -n "$HTTP_BIND" ]; then
  : "${QJANUS_ALLOW_NONPRIVATE_BIND:=$(get_env QJANUS_ALLOW_NONPRIVATE_BIND)}"
  export QJANUS_ALLOW_NONPRIVATE_BIND
  qjanus_check_http_bind "$HTTP_BIND" || die "--http-bind $HTTP_BIND is not acceptable"
fi

# one installer at a time (a second run would race on releases/ and on the unit)
mkdir -p "$(dirname "$LOCKF")"
exec 9> "$LOCKF"
flock -n 9 || die "another qjanus install.sh is running"

# complete_releases [healthy]: the fully installed releases, newest first (a *.tmp directory is an
# install that was interrupted, a directory without .complete or bin/janus is not a release);
# "healthy" keeps the ones that have come up and answered on this node (what --rollback may pick:
# never a release that was installed but did not start)
complete_releases() {
  local d
  while read -r d; do
    [ -e "$OPT/releases/$d/.complete" ] && [ -x "$OPT/releases/$d/bin/janus" ] || continue
    [ "${1:-}" != healthy ] || [ -e "$OPT/releases/$d/.healthy" ] || continue
    echo "$d"
  done < <(ls -1t "$OPT/releases" 2> /dev/null | grep -Ev '\.tmp$' || true)
}
release_ok() { [ -e "$1/.complete" ] && [ -x "$1/bin/janus" ] && [ -f "$1/share/qjanus/systemd/qjanus.service" ]; }

# activate DIR: `current` -> DIR (atomically) and the unit that release ships
activate() {
  ln -sfn "$1" "$OPT/current.new" && mv -Tf "$OPT/current.new" "$OPT/current"
  if ! cmp -s "$1/share/qjanus/systemd/qjanus.service" "$UNIT" 2> /dev/null; then
    install -m 0644 "$1/share/qjanus/systemd/qjanus.service" "$UNIT"
  fi
  systemctl daemon-reload
}

bind_host() {
  local h
  h=$(get_env QJANUS_HTTP_BIND)
  case "$h" in 0.0.0.0|::|"") h=127.0.0.1 ;; esac
  echo "$h"
}
# restart_service: a unit that hit its start limit refuses a plain restart, so clear that first
restart_service() {
  systemctl reset-failed qjanus.service > /dev/null 2>&1 || true
  systemctl restart qjanus.service || true
}
# wait_healthy SECONDS: active, and the server API accepts connections (the unit's ExecStartPost
# has already insisted on both APIs answering before the service counts as started)
wait_healthy() {
  local i n=$(( $1 * 2 )) host
  host=$(bind_host)
  for ((i = 0; i < n; i++)); do
    if systemctl is-active --quiet qjanus.service && (exec 3<> "/dev/tcp/$host/8088") 2> /dev/null; then return 0; fi
    sleep 0.5
  done
  return 1
}
show_journal() { journalctl -u qjanus.service -n 30 --no-pager >&2 || true; }

if [ "$MODE" = uninstall ]; then
  log "stopping and removing the qjanus service"
  systemctl disable --now qjanus.service 2>/dev/null || true
  systemctl reset-failed qjanus.service > /dev/null 2>&1 || true
  rm -f "$UNIT"
  systemctl daemon-reload
  rm -rf "$OPT"
  if [ "$PURGE" = 1 ]; then
    rm -rf "$ETC"
    log "purged $ETC (DTLS key and secrets are gone: the next install creates a NEW fingerprint)"
  else
    log "kept $ETC (DTLS key, secrets, settings); --purge removes them"
  fi
  exit 0
fi

command -v systemctl > /dev/null || die "systemd is required"

if [ "$MODE" = rollback ]; then
  [ -L "$OPT/current" ] || die "no current release"
  cur=$(basename "$(readlink -f "$OPT/current")")
  prev=$(complete_releases healthy | grep -vx "$cur" | head -1 || true)
  [ -n "$prev" ] || die "no previous release that ever ran on this node to roll back to"
  log "rolling back $cur -> $prev"
  activate "$OPT/releases/$prev"
  touch "$OPT/releases/$prev"
  restart_service
  if ! wait_healthy 15; then
    show_journal
    die "qjanus did not come back after the rollback (journalctl -u qjanus)"
  fi
  fingerprint
  exit 0
fi

# ---- preflight
[ -x "$SELF/bin/janus" ] && [ -f "$SELF/VERSION" ] || die "run this script from the unpacked release directory"
command -v openssl > /dev/null || die "openssl is needed (only to create the DTLS certificate; janus does not use it)"
if [ -f "$SELF/apt-deps.txt" ]; then
  missing=""
  while read -r pkg; do
    [ -n "$pkg" ] || continue
    dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q 'install ok installed' || missing="$missing $pkg"
  done < "$SELF/apt-deps.txt"
  if [ -n "$missing" ]; then
    if [ "$INSTALL_DEPS" = 1 ]; then
      log "installing runtime packages:$missing"
      DEBIAN_FRONTEND=noninteractive apt-get update -qq >&2
      # shellcheck disable=SC2086
      DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends $missing >&2
    else
      die "missing runtime packages:$missing   (re-run with --install-deps, or: apt-get install$missing)"
    fi
  fi
fi
"$SELF/bin/janus" --version > /dev/null 2>&1 || die "bin/janus does not run here (Ubuntu 24.04 x86_64 with glibc >= 2.39 is required)"

REL=$(cat "$SELF/VERSION")
[[ $REL =~ ^[A-Za-z0-9._+-]+$ ]] || die "invalid VERSION file"
CHANGED=0

# what runs now, so that a failed upgrade can be undone
PREV_LINK=""
[ -L "$OPT/current" ] && PREV_LINK=$(readlink "$OPT/current")
if [ -e "$ENVF" ]; then
  ENV_BACKUP=$(mktemp "$ETC/.qjanus.env.prev.XXXXXX")
  cp -p "$ENVF" "$ENV_BACKUP"
fi

# ---- release directory + current symlink
mkdir -p "$OPT/releases"
chmod 0755 "$OPT" "$OPT/releases"
rm -rf "$OPT"/releases/*.tmp       # an interrupted earlier install
DEST=$OPT/releases/$REL
if [ "$SELF" != "$DEST" ]; then
  if [ -e "$DEST/.complete" ]; then
    # the same version name must be the same build: never keep running an old tree silently
    cmp -s "$SELF/BUILDINFO.json" "$DEST/BUILDINFO.json" 2> /dev/null \
      || die "release $REL is already installed with different content (BUILDINFO.json differs): give the new build a new version"
  else
    log "installing release $REL"
    rm -rf "$DEST.tmp"
    cp -a "$SELF" "$DEST.tmp"
    chown -R root:root "$DEST.tmp"
    chmod -R go-w "$DEST.tmp"
    touch "$DEST.tmp/.complete"
    rm -rf "$DEST"
    mv "$DEST.tmp" "$DEST"
  fi
  touch "$DEST"   # release order = order of installation (the old-release pruning and --rollback use it)
fi
release_ok "$DEST" || die "$DEST is not a complete release"

# ---- configuration: DTLS certificate (fixed per node), env file (secrets + node settings)
mkdir -p "$ETC"
chmod 0755 "$ETC"
if [ -e "$ETC/dtls.key" ] || [ -e "$ETC/dtls.crt" ]; then
  [ -s "$ETC/dtls.key" ] && [ -s "$ETC/dtls.crt" ] || die "$ETC/dtls.key and dtls.crt must both exist (or both be absent)"
else
  log "generating the ECDSA P-256 DTLS certificate (10 years)"
  ( umask 077
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days "$DTLS_DAYS" \
      -subj "/CN=qjanus" -keyout "$ETC/dtls.key.tmp" -out "$ETC/dtls.crt.tmp" 2> /dev/null )
  chmod 0600 "$ETC/dtls.key.tmp"; chmod 0644 "$ETC/dtls.crt.tmp"
  mv "$ETC/dtls.key.tmp" "$ETC/dtls.key"; mv "$ETC/dtls.crt.tmp" "$ETC/dtls.crt"
  CHANGED=1
fi
chown root:root "$ETC/dtls.key" "$ETC/dtls.crt"
chmod 0600 "$ETC/dtls.key"; chmod 0644 "$ETC/dtls.crt"

if [ ! -e "$ENVF" ]; then
  log "creating $ENVF with generated secrets"
  ( umask 077; : > "$ENVF" )
  CHANGED=1
fi
chown root:root "$ENVF"; chmod 0600 "$ENVF"
[ -n "$(get_env QJANUS_TOKEN_SECRET)" ] || { set_env QJANUS_TOKEN_SECRET "$(openssl rand -hex 32)"; CHANGED=1; }
[ -n "$(get_env QJANUS_ADMIN_KEY)" ] || { set_env QJANUS_ADMIN_KEY "$(openssl rand -hex 32)"; CHANGED=1; }
if [ -n "$HTTP_BIND" ] && [ "$(get_env QJANUS_HTTP_BIND)" != "$HTTP_BIND" ]; then
  set_env QJANUS_HTTP_BIND "$HTTP_BIND"; CHANGED=1
fi
[ -n "$(get_env QJANUS_HTTP_BIND)" ] || { set_env QJANUS_HTTP_BIND 127.0.0.1; CHANGED=1; }

# ---- the release and its unit
if [ "$(readlink "$OPT/current" 2>/dev/null || true)" != "$DEST" ] || ! cmp -s "$SELF/share/qjanus/systemd/qjanus.service" "$UNIT" 2> /dev/null; then
  CHANGED=1
fi
activate "$DEST"
systemctl enable qjanus.service > /dev/null 2>&1

# ---- start / restart
if [ "$START" = 1 ]; then
  if ! systemctl is-active --quiet qjanus.service; then
    restart_service
  elif [ "$CHANGED" = 1 ]; then
    log "restarting qjanus (release, unit or settings changed)"
    restart_service
  fi
  if wait_healthy 10; then
    touch "$DEST/.healthy"     # this release has come up on this node: a valid --rollback target from now on
  else
    show_journal
    # put back what ran before: the previous release with its unit, the previous env file
    recovered=0
    if [ -n "$PREV_LINK" ] && [ "$PREV_LINK" != "$DEST" ] && release_ok "$PREV_LINK"; then
      log "release $REL did not come up: putting back $(basename "$PREV_LINK")"
      activate "$PREV_LINK"
      recovered=1
    fi
    if [ -n "$ENV_BACKUP" ] && ! cmp -s "$ENV_BACKUP" "$ENVF"; then
      log "putting back the previous $ENVF"
      install -m 0600 -o root -g root "$ENV_BACKUP" "$ENVF"
      recovered=1
    fi
    if [ "$recovered" = 1 ]; then
      restart_service
      if wait_healthy 15; then
        die "qjanus $REL did not start; the previous release and settings are running again (journalctl -u qjanus)"
      fi
      show_journal
      die "qjanus $REL did not start, and neither did the previous release and settings (journalctl -u qjanus)"
    fi
    die "qjanus did not start (see the journal above)"
  fi
  log "qjanus $REL is running (server API on $(bind_host):8088, client API on 127.0.0.1:8188)"
else
  log "installed $REL (not started)"
fi

# ---- old releases: keep the newest N (the current one is always kept)
cur=$(basename "$(readlink -f "$OPT/current")")
{ complete_releases | grep -vx "$cur" || true; } | tail -n +"$KEEP" | while read -r old; do
  if [ -n "$old" ]; then log "removing old release $old"; rm -rf "${OPT:?}/releases/$old"; fi
done
fingerprint
