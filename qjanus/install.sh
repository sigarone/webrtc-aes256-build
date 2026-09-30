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
set -euo pipefail

SELF=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
OPT=/opt/qjanus
ETC=/etc/qjanus
UNIT=/etc/systemd/system/qjanus.service
ENVF=$ETC/qjanus.env
KEEP=3
HTTP_BIND=""
INSTALL_DEPS=0
START=1
MODE=install
PURGE=0
DTLS_DAYS=3650

log() { echo "qjanus-install: $*" >&2; }
die() { echo "qjanus-install: ERROR: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --http-bind) [ $# -ge 2 ] || die "--http-bind needs an address"; HTTP_BIND=$2; shift 2 ;;
    --install-deps) INSTALL_DEPS=1; shift ;;
    --no-start) START=0; shift ;;
    --keep-releases) [ $# -ge 2 ] || die "--keep-releases needs a number"; KEEP=$2; shift 2 ;;
    --rollback) MODE=rollback; shift ;;
    --uninstall) MODE=uninstall; shift ;;
    --purge) PURGE=1; shift ;;
    -h|--help) sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 0 ;;
    *) die "unknown option $1" ;;
  esac
done
[ "$(id -u)" = 0 ] || die "run as root"
[[ $KEEP =~ ^[0-9]+$ ]] && [ "$KEEP" -ge 2 ] || die "--keep-releases must be a number >= 2"
if [ -n "$HTTP_BIND" ]; then
  [[ $HTTP_BIND =~ ^[0-9A-Fa-f:.]{2,45}$ ]] || die "--http-bind must be an IPv4 or IPv6 address"
fi

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

if [ "$MODE" = uninstall ]; then
  log "stopping and removing the qjanus service"
  systemctl disable --now qjanus.service 2>/dev/null || true
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
  prev=$(ls -1t "$OPT/releases" | grep -vx "$cur" | head -1 || true)
  [ -n "$prev" ] || die "no previous release to roll back to"
  log "rolling back $cur -> $prev"
  ln -sfn "$OPT/releases/$prev" "$OPT/current.new" && mv -Tf "$OPT/current.new" "$OPT/current"
  touch "$OPT/releases/$prev"
  systemctl restart qjanus.service
  systemctl is-active --quiet qjanus.service || die "qjanus did not come back after the rollback (journalctl -u qjanus)"
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

# ---- release directory + current symlink
mkdir -p "$OPT/releases"
chmod 0755 "$OPT" "$OPT/releases"
DEST=$OPT/releases/$REL
if [ "$SELF" != "$DEST" ]; then
  if [ ! -e "$DEST/.complete" ]; then
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
if [ "$(readlink "$OPT/current" 2>/dev/null || true)" != "$DEST" ]; then
  ln -sfn "$DEST" "$OPT/current.new" && mv -Tf "$OPT/current.new" "$OPT/current"
  CHANGED=1
fi

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

# ---- the unit
if ! cmp -s "$SELF/share/qjanus/systemd/qjanus.service" "$UNIT" 2> /dev/null; then
  install -m 0644 "$SELF/share/qjanus/systemd/qjanus.service" "$UNIT"
  CHANGED=1
fi
systemctl daemon-reload
systemctl enable qjanus.service > /dev/null 2>&1

# ---- old releases: keep the newest N (the current one is always kept)
cur=$(basename "$(readlink -f "$OPT/current")")
{ ls -1t "$OPT/releases" | grep -vx "$cur" || true; } | tail -n +"$KEEP" | while read -r old; do
  if [ -n "$old" ]; then log "removing old release $old"; rm -rf "${OPT:?}/releases/$old"; fi
done

# ---- start / restart
if [ "$START" = 1 ]; then
  if ! systemctl is-active --quiet qjanus.service; then
    systemctl restart qjanus.service
  elif [ "$CHANGED" = 1 ]; then
    log "restarting qjanus (release, unit or settings changed)"
    systemctl restart qjanus.service
  fi
  host=$(get_env QJANUS_HTTP_BIND)
  case "$host" in 0.0.0.0|::|"") host=127.0.0.1 ;; esac
  ok=0
  for _ in $(seq 1 30); do
    if systemctl is-active --quiet qjanus.service && (exec 3<> "/dev/tcp/$host/8088") 2> /dev/null; then ok=1; break; fi
    sleep 0.5
  done
  if [ "$ok" != 1 ]; then
    journalctl -u qjanus.service -n 30 --no-pager >&2 || true
    die "qjanus did not start (see the journal above)"
  fi
  log "qjanus $REL is running (server API on $host:8088, client API on 127.0.0.1:8188)"
else
  log "installed $REL (not started)"
fi
fingerprint
