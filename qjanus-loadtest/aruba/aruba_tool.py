#!/usr/bin/env python3
"""aruba_tool.py - on-node helper for the qjanus load test against a PRODUCTION box.

Runs ON the node (Python 3 standard library only). It exists so that the core token secret and
the videoroom admin_key never leave the node:

  mint     print a JSON array of pre-minted Janus core session tokens (one per bot, short expiry)
  rooms    create | destroy | list the load-test rooms (ids derived from the seed exactly like
           src/ids.mjs, spec room parameters exactly like src/janus-admin.mjs)
  gate     one-shot "may the run start" check (window, no call, no group call, quiet period, load)
  guard    watchdog loop during the run: samples the production server and the box, and on any
           breach destroys the load-test rooms and writes an ABORT file (it never touches a service)

Secrets are read from the node's own env files, never printed, never put on a command line.
The seed arrives in QJANUS_LOADTEST_SEED, in the file QJANUS_LOADTEST_SEED_FILE or on stdin (first line). Output never contains tokens,
secrets, full room ids, addresses or user identifiers.

Exit codes: 0 ok, 1 failure, 2 usage, 3 gate closed, 4 guard aborted.
"""

import argparse
import base64
import csv
import hashlib
import hmac
import json
import os
import re
import secrets as pysecrets
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

QJANUS_ENV = os.environ.get("QJANUS_ENV_FILE", "/etc/qjanus/qjanus.env")
BCRYPTO_ENV = os.environ.get("BCRYPTO_ENV_FILE", "/etc/bcrypto/secrets.env")
BCRYPTO_URL = os.environ.get("BCRYPTO_URL", "http://127.0.0.1:8443")
BCRYPTO_UNIT = os.environ.get("BCRYPTO_UNIT", "bcrypto-server")
PLUGIN = "janus.plugin.videoroom"
MAX_TTL_SEC = 3 * 3600
MAX_TOKENS = 512
HARNESS_ROOM_SCAN = 64  # rooms of the seed are looked for among k = 0 .. 63

# thresholds (the owner's abort rules for the production run)
LOAD1_ABORT = 3.5
LOAD1_START_MAX = 1.5
QUIET_MINUTES = 15
BC_CPU_ABORT_PCT = 40.0  # bcrypto-server, percent of ONE core, CONSECUTIVE samples in a row
HEALTH_ABS_MS = 150.0
HEALTH_REL = 6.0
CONSECUTIVE = 3
ROOMS_UNREADABLE_CHECKS = 12  # guard checks (5 s each) in a row without a readable Janus room list

# Janus videoroom parameters of spec section 3 (mirror of SPEC_ROOM_DEFAULTS in src/janus-admin.mjs)
SPEC_ROOM_DEFAULTS = {
    "is_private": True, "bitrate": 1500000, "fir_freq": 10, "audiocodec": "opus", "videocodec": "vp8",
    "opus_fec": True, "opus_dtx": False, "audiolevel_ext": False, "audiolevel_event": False,
    "videoorient_ext": False, "playoutdelay_ext": False, "transport_wide_cc_ext": True,
    "record": False, "lock_record": True, "require_pvtid": True, "require_e2ee": True,
    "notify_joining": False,
}
ROOM_EXISTS = 427
NO_SUCH_ROOM = 426

CALL_LINE_RE = re.compile(r'msg="?(call_offer|call_answer|call_accepted|call_ready|group[_-]call|call-state snapshot saved active_calls=[1-9])')
CALL_COUNTER_TYPES = ("call_offer", "call_answer", "call_accepted")


class ToolError(Exception):
    code = None


# ----------------------------------------------------------------------------- helpers

def read_env_file(path):
    out = {}
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                v = v.strip()
                if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                    v = v[1:-1]
                out[k.strip()] = v
    except OSError:
        raise ToolError("cannot read an environment file of the node")
    return out


def janus_base(env):
    bind = env.get("QJANUS_HTTP_BIND", "")
    if bind in ("", "0.0.0.0", "::"):
        bind = "127.0.0.1"
    return "http://%s:%s/janus" % (bind, os.environ.get("QJANUS_HTTP_PORT", "8088"))


def mint_token(secret, ttl_sec, now=None, plugins=(PLUGIN,)):
    """Same format as src/token.mjs mintSessionToken: "<expiry>,janus,<plugin>:<base64 HMAC-SHA256>"."""
    if not secret:
        raise ToolError("empty token secret")
    if not (0 < ttl_sec <= MAX_TTL_SEC):
        raise ToolError("ttl must be between 1 and %d seconds" % MAX_TTL_SEC)
    expiry = int((time.time() if now is None else now) + ttl_sec)
    data = ",".join([str(expiry), "janus"] + list(plugins))
    sig = base64.b64encode(hmac.new(secret.encode(), data.encode(), hashlib.sha256).digest()).decode()
    return "%s:%s" % (data, sig)


def _derive(seed, label):
    return hmac.new(seed.encode(), label.encode(), hashlib.sha256).hexdigest()


def room_id(seed, k):
    return _derive(seed, "room|%d" % k)[:32]


def room_secret(seed, room):
    return _derive(seed, "secret|%s" % room)


def pseudonym(seed, k, i):
    return _derive(seed, "pseudo|%d|%d" % (k, i))[:32]


def join_token(seed, k, i):
    return "%s:%s" % (pseudonym(seed, k, i), _derive(seed, "join|%d|%d" % (k, i))[:32])


def read_seed():
    """The run seed: QJANUS_LOADTEST_SEED, else the file QJANUS_LOADTEST_SEED_FILE, else the first line of stdin.
    (The seed only derives test room ids; it is random per run and is no production secret.)"""
    seed = os.environ.get("QJANUS_LOADTEST_SEED", "")
    path = os.environ.get("QJANUS_LOADTEST_SEED_FILE")
    if not seed and path:
        try:
            with open(path, "r", encoding="ascii") as fh:
                seed = fh.readline().strip()
        except OSError:
            raise ToolError("cannot read the seed file")
    if not seed:
        seed = sys.stdin.readline().strip()
    if len(seed) < 12:
        raise ToolError("a seed of at least 12 characters is required (QJANUS_LOADTEST_SEED, QJANUS_LOADTEST_SEED_FILE or stdin)")
    return seed


def http_json(url, payload=None, headers=None, timeout=8):
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data, headers=dict({"content-type": "application/json"}, **(headers or {})))
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode())
    except (urllib.error.URLError, OSError, ValueError):
        raise ToolError("request to the local service failed")


class Janus:
    """Minimal Janus HTTP client (admin only): one session + videoroom handle, a fresh token per request."""

    def __init__(self, env, secret=None, admin_key=None):
        self.base = janus_base(env)
        self.secret = secret or env.get("QJANUS_TOKEN_SECRET", "")
        self.admin_key = admin_key or env.get("QJANUS_ADMIN_KEY", "")
        if not self.secret or not self.admin_key:
            raise ToolError("the node env file lacks the token secret or the admin key")
        self.sid = None
        self.hid = None

    def _post(self, path, payload):
        payload = dict(payload, transaction=pysecrets.token_hex(12), token=mint_token(self.secret, 600))
        reply = http_json(self.base + path, payload)
        if reply.get("janus") == "error":
            e = ToolError("janus error %s" % reply.get("error", {}).get("code"))
            e.code = reply.get("error", {}).get("code")
            raise e
        return reply

    def open(self):
        self.sid = self._post("", {"janus": "create"})["data"]["id"]
        self.hid = self._post("/%d" % self.sid, {"janus": "attach", "plugin": PLUGIN})["data"]["id"]

    def close(self):
        try:
            if self.sid:
                self._post("/%d" % self.sid, {"janus": "destroy"})
        except ToolError:
            pass
        self.sid = self.hid = None

    def request(self, body):
        if self.sid is None:
            self.open()
        reply = self._post("/%d/%d" % (self.sid, self.hid), {"janus": "message", "body": body})
        data = reply.get("plugindata", {}).get("data", {})
        if "error_code" in data:
            e = ToolError("videoroom %s: code %s" % (body.get("request"), data["error_code"]))
            e.code = data["error_code"]
            raise e
        return data

    def create_room(self, seed, k, size):
        """Returns True when the room existed already."""
        room = room_id(seed, k)
        secret = room_secret(seed, room)
        tokens = [join_token(seed, k, i) for i in range(size)]
        body = dict(SPEC_ROOM_DEFAULTS, request="create", room=room, secret=secret, publishers=size,
                    allowed=tokens, admin_key=self.admin_key)
        try:
            self.request(body)
            return False
        except ToolError as e:
            if e.code != ROOM_EXISTS:
                raise
        # exists: top the ACL up and enable it, like createRooms() does
        self.request({"request": "allowed", "room": room, "secret": secret, "action": "add", "allowed": tokens})
        self.request({"request": "allowed", "room": room, "secret": secret, "action": "enable"})
        return True

    def destroy_room(self, seed, k):
        """Returns True when a room was destroyed, False when there was none."""
        room = room_id(seed, k)
        try:
            self.request({"request": "destroy", "room": room, "secret": room_secret(seed, room)})
            return True
        except ToolError as e:
            if e.code == NO_SUCH_ROOM:
                return False
            raise

    def list_rooms(self):
        return self.request({"request": "list", "admin_key": self.admin_key}).get("list", [])


# ----------------------------------------------------------------------------- production probes

def parse_metrics(text):
    """The few Prometheus values the gate needs. Missing ones stay None (the callers fail closed)."""
    out = {"active_calls": None, "total_calls": None, "call_msgs": 0.0, "group_msgs": 0.0}
    for line in text.splitlines():
        if line.startswith("bcrypto_active_calls "):
            out["active_calls"] = float(line.split()[1])
        elif line.startswith("bcrypto_total_calls "):
            out["total_calls"] = float(line.split()[1])
        elif line.startswith("bcrypto_ws_messages_by_type_total{"):
            m = re.match(r'bcrypto_ws_messages_by_type_total\{type="([^"]+)"\}\s+([0-9.eE+]+)', line)
            if not m:
                continue
            if m.group(1) in CALL_COUNTER_TYPES:
                out["call_msgs"] += float(m.group(2))
            elif "group" in m.group(1):
                out["group_msgs"] += float(m.group(2))
    return out


class Probes:
    """Read-only probes of the production server and the box. Unknown values are None (fail closed)."""

    def __init__(self, env=None):
        self.env = env or read_env_file(QJANUS_ENV)
        try:
            self.bc_key = read_env_file(BCRYPTO_ENV).get("BCRYPTO_ADMIN_API_KEY", "")
        except ToolError:
            self.bc_key = ""
        self._cpu_prev = None

    def metrics(self):
        if not self.bc_key:
            return None
        req = urllib.request.Request(BCRYPTO_URL + "/metrics", headers={"X-Admin-Key": self.bc_key})
        try:
            with urllib.request.urlopen(req, timeout=4) as resp:
                return parse_metrics(resp.read().decode())
        except (urllib.error.URLError, OSError, ValueError):
            return None

    def health(self):
        """(http status, milliseconds) of /api/v1/health on the server itself (no proxy in between)."""
        t0 = time.monotonic()
        try:
            with urllib.request.urlopen(BCRYPTO_URL + "/api/v1/health", timeout=4) as resp:
                resp.read()
                return resp.status, (time.monotonic() - t0) * 1000
        except urllib.error.HTTPError as e:
            return e.code, (time.monotonic() - t0) * 1000
        except (urllib.error.URLError, OSError):
            return 0, (time.monotonic() - t0) * 1000

    def load1(self):
        try:
            with open("/proc/loadavg", "r", encoding="ascii") as fh:
                return float(fh.read().split()[0])
        except (OSError, ValueError):
            return None

    def bc_pid(self):
        try:
            out = subprocess.run(["systemctl", "show", "-p", "MainPID", "--value", BCRYPTO_UNIT],
                                 capture_output=True, text=True, timeout=5).stdout.strip()
            return int(out) or None
        except (OSError, ValueError, subprocess.SubprocessError):
            return None

    def bc_cpu_pct(self):
        """CPU of the bcrypto server process in percent of one core since the previous call (None on the first)."""
        pid = self.bc_pid()
        if pid is None:
            return None
        try:
            with open("/proc/%d/stat" % pid, "r", encoding="ascii") as fh:
                f = fh.read().rsplit(")", 1)[1].split()
            ticks = int(f[11]) + int(f[12])
        except (OSError, ValueError, IndexError):
            return None
        now = time.monotonic()
        prev, self._cpu_prev = self._cpu_prev, (pid, ticks, now)
        if prev is None or prev[0] != pid or now <= prev[2]:
            return None
        return 100.0 * ((ticks - prev[1]) / os.sysconf("SC_CLK_TCK")) / (now - prev[2])

    def recent_call_lines(self, minutes=QUIET_MINUTES):
        """Call / group-call lines in the production journal of the last minutes (None = unreadable)."""
        try:
            out = subprocess.run(["journalctl", "-u", BCRYPTO_UNIT, "--since", "-%dmin" % minutes, "--no-pager", "-o", "cat"],
                                 capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            return None
        if out.returncode != 0:
            return None
        return sum(1 for line in out.stdout.splitlines() if CALL_LINE_RE.search(line))

    def rooms(self, seed):
        """(foreign rooms, participants in them, rooms of this seed, participants in those)."""
        janus = Janus(self.env)
        try:
            rooms = janus.list_rooms()
        finally:
            janus.close()
        mine = set(room_id(seed, k) for k in range(HARNESS_ROOM_SCAN))
        fr = [r for r in rooms if r.get("room") not in mine]
        hr = [r for r in rooms if r.get("room") in mine]
        return (len(fr), sum(int(r.get("num_participants", 0)) for r in fr),
                len(hr), sum(int(r.get("num_participants", 0)) for r in hr))


# ----------------------------------------------------------------------------- window + gate

def rome_now(now=None):
    from zoneinfo import ZoneInfo
    return (now or datetime.now(timezone.utc)).astimezone(ZoneInfo("Europe/Rome"))


def in_window(now=None):
    """The run window: 23:30 .. 06:00 Europe/Rome."""
    t = rome_now(now)
    minutes = t.hour * 60 + t.minute
    return minutes >= 23 * 60 + 30 or minutes < 6 * 60


def evaluate_gate(m, quiet_lines, load1, health, rooms, window_ok):
    """Pure decision function (unit tested): the reasons the run may NOT start (empty list = go)."""
    why = []
    if not window_ok:
        why.append("outside the run window (23:30-06:00 Europe/Rome)")
    if m is None:
        why.append("production metrics unreadable")
    else:
        if m["active_calls"] is None or m["active_calls"] != 0:
            why.append("bcrypto_active_calls is not 0")
        if m["total_calls"] is None:
            why.append("bcrypto_total_calls unreadable")
    if quiet_lines is None:
        why.append("production journal unreadable")
    elif quiet_lines > 0:
        why.append("call activity in the last %d minutes" % QUIET_MINUTES)
    if load1 is None or load1 > LOAD1_START_MAX:
        why.append("load average above %.1f" % LOAD1_START_MAX)
    if health is None or health[0] != 200:
        why.append("health endpoint not healthy")
    if rooms is None:
        why.append("janus rooms unreadable")
    else:
        if rooms[1] > 0:
            why.append("a group call is active (a foreign room has participants)")
        if rooms[2] > 0:
            why.append("rooms of this seed already exist")
    return why


def cmd_gate(args):
    seed = read_seed()
    p = Probes()
    m = p.metrics()
    try:
        rooms = p.rooms(seed)
    except ToolError:
        rooms = None
    why = evaluate_gate(m, p.recent_call_lines(), p.load1(), p.health(), rooms, in_window() or args.ignore_window)
    print(json.dumps({"go": not why, "reasons": why, "rome": rome_now().strftime("%Y-%m-%d %H:%M"),
                      "active_calls": None if m is None else m["active_calls"]}))
    return 0 if not why else 3


# ----------------------------------------------------------------------------- guard

class GuardState:
    """Consecutive-breach bookkeeping, separated from the I/O so the decisions are testable."""

    def __init__(self, baseline, baseline_health_ms):
        self.base = baseline
        self.health_limit = max(HEALTH_ABS_MS, HEALTH_REL * baseline_health_ms)
        self.cpu_run = 0
        self.health_run = 0
        self.foreign_n = None
        self.rooms_none = 0

    def check(self, s):
        """s: dict(load1, bc_cpu, health=(status, ms), metrics, rooms, window_ok, manual). A reason, or None to go on."""
        if s.get("manual"):
            return "manual stop file"
        if not s["window_ok"]:
            return "run window over"
        if s["load1"] is not None and s["load1"] > LOAD1_ABORT:
            return "load average above %.1f" % LOAD1_ABORT
        m = s["metrics"]
        if m is None:
            return "production metrics unreadable"
        if m["active_calls"] is None or m["active_calls"] > 0:
            return "a real call is active"
        if m["total_calls"] != self.base["total_calls"] or m["call_msgs"] != self.base["call_msgs"] or m["group_msgs"] != self.base["group_msgs"]:
            return "a real call or group call started"
        rooms = s["rooms"]
        if rooms is None:
            self.rooms_none += 1
            if self.rooms_none >= ROOMS_UNREADABLE_CHECKS:
                return "janus room list unreadable"
        else:
            self.rooms_none = 0
            if rooms[1] > 0:
                return "a group call is active"
            if self.foreign_n is None:
                self.foreign_n = rooms[0]
            elif rooms[0] > self.foreign_n:
                return "a foreign room appeared"
        status, ms = s["health"]
        self.health_run = self.health_run + 1 if (status != 200 or ms > self.health_limit) else 0
        if self.health_run >= CONSECUTIVE:
            return "production health degraded"
        cpu = s["bc_cpu"]
        self.cpu_run = self.cpu_run + 1 if (cpu is not None and cpu > BC_CPU_ABORT_PCT) else 0
        if self.cpu_run >= CONSECUTIVE:
            return "production server CPU high"
        return None


def destroy_all(env, seed):
    n = 0
    j = Janus(env)
    try:
        for k in range(HARNESS_ROOM_SCAN):
            n += 1 if j.destroy_room(seed, k) else 0
    finally:
        j.close()
    return n


def cmd_guard(args):
    try:
        return _guard(args)
    finally:
        try:
            os.remove(os.path.join(args.out, "guard.pid"))
        except OSError:
            pass


def _guard(args):
    seed = read_seed()
    os.makedirs(args.out, mode=0o700, exist_ok=True)
    abort_file = os.path.join(args.out, "ABORT")
    stop_file = os.path.join(args.out, "STOP")
    for f in (abort_file, stop_file):
        if os.path.exists(f):
            os.remove(f)
    pid_file = os.path.join(args.out, "guard.pid")
    with open(pid_file, "w") as pf:
        pf.write("%d\n" % os.getpid())
    p = Probes()
    m0 = p.metrics()
    if m0 is None or m0["active_calls"] is None or m0["total_calls"] is None:
        raise ToolError("cannot start the guard: production metrics unreadable")
    health0 = sorted(p.health()[1] for _ in range(5))[2]
    st = GuardState(m0, health0)
    p.bc_cpu_pct()  # prime the CPU delta
    out_csv = os.path.join(args.out, "guard.csv")
    new = not os.path.exists(out_csv)
    fh = open(out_csv, "a", newline="")
    w = csv.writer(fh)
    if new:
        w.writerow(["ts", "iso", "load1", "bc_cpu_pct", "health_status", "health_ms", "active_calls", "foreign_rooms",
                    "foreign_participants", "harness_rooms", "harness_participants"])
    print("guard started; baseline health %.1f ms, limit %.1f ms" % (health0, st.health_limit), flush=True)
    last_rooms, last_rooms_t = None, 0.0
    reason = None
    while reason is None:
        time.sleep(args.interval)
        now = time.time()
        if now - last_rooms_t >= 15:
            try:
                last_rooms = p.rooms(seed)
            except ToolError:
                last_rooms = None
            last_rooms_t = now
        status, ms = p.health()
        s = {"load1": p.load1(), "bc_cpu": p.bc_cpu_pct(), "health": (status, ms), "metrics": p.metrics(),
             "rooms": last_rooms, "window_ok": in_window() or args.ignore_window, "manual": os.path.exists(stop_file)}
        fr = last_rooms or ("", "", "", "")
        w.writerow(["%.1f" % now, datetime.fromtimestamp(now, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                    s["load1"], "" if s["bc_cpu"] is None else "%.1f" % s["bc_cpu"], status, "%.1f" % ms,
                    "" if s["metrics"] is None else s["metrics"]["active_calls"], fr[0], fr[1], fr[2], fr[3]])
        fh.flush()
        reason = st.check(s)
    # a breach: cut the load first (destroy the rooms of the seed), then record why. A "manual stop" is the
    # normal end of a run, the controller destroys the rooms itself, so it is not an abort.
    destroyed = 0
    try:
        destroyed = destroy_all(p.env, seed)
    except ToolError:
        pass
    if reason == "manual stop file":
        print("guard stopped (rooms destroyed: %d)" % destroyed, flush=True)
        return 0
    with open(abort_file, "w") as af:
        af.write("%s\nrooms destroyed: %d\nat %s\n" % (reason, destroyed, datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")))
    print("GUARD ABORT: %s (rooms destroyed: %d)" % (reason, destroyed), flush=True)
    return 4


# ----------------------------------------------------------------------------- mint + rooms

def cmd_mint(args):
    if not (1 <= args.count <= MAX_TOKENS):
        raise ToolError("count must be between 1 and %d" % MAX_TOKENS)
    secret = read_env_file(QJANUS_ENV).get("QJANUS_TOKEN_SECRET", "")
    now = time.time()
    sys.stdout.write(json.dumps([mint_token(secret, args.ttl_sec, now=now) for _ in range(args.count)]))
    sys.stdout.flush()
    return 0


def cmd_rooms(args):
    seed = read_seed()
    env = read_env_file(QJANUS_ENV)
    if args.action == "list":
        fn, fp, hn, hp = Probes(env).rooms(seed)
        print(json.dumps({"foreign_rooms": fn, "foreign_participants": fp, "harness_rooms": hn, "harness_participants": hp}))
        return 0
    j = Janus(env)
    try:
        if args.action == "create":
            if not (1 <= args.rooms <= HARNESS_ROOM_SCAN):
                raise ToolError("--rooms must be between 1 and %d" % HARNESS_ROOM_SCAN)
            made = [j.create_room(seed, k, args.size) for k in range(args.rooms)]
            print("created %d rooms (%d existed already)" % (len(made), sum(1 for x in made if x)))
        else:
            gone = [j.destroy_room(seed, k) for k in range(HARNESS_ROOM_SCAN)]
            print("destroyed %d rooms" % sum(1 for x in gone if x))
    finally:
        j.close()
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="qjanus load test helper that runs on the node")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("mint")
    s.add_argument("--count", type=int, required=True)
    s.add_argument("--ttl-sec", type=int, default=MAX_TTL_SEC)
    s.set_defaults(fn=cmd_mint)
    s = sub.add_parser("rooms")
    s.add_argument("action", choices=["create", "destroy", "list"])
    s.add_argument("--rooms", type=int, default=0)
    s.add_argument("--size", type=int, default=8)
    s.set_defaults(fn=cmd_rooms)
    s = sub.add_parser("gate")
    s.add_argument("--ignore-window", action="store_true", help="dry run outside the window (every other gate is still reported)")
    s.set_defaults(fn=cmd_gate)
    s = sub.add_parser("guard")
    s.add_argument("--out", required=True)
    s.add_argument("--interval", type=float, default=5.0)
    s.add_argument("--ignore-window", action="store_true")
    s.set_defaults(fn=cmd_guard)
    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except ToolError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
