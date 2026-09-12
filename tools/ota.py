#!/usr/bin/env python3
"""Update the board over WiFi, and confirm it actually took.

    python tools/ota.py --host frank.local
    python tools/ota.py --host 192.168.1.50 --env gray_rtc_ota

Why this exists rather than `pio run -t upload`: espota.py reports failure for
updates that have already succeeded, and it does so most of the time on a
weak link.

The cause is ack bookkeeping, not the transfer.  espota performs exactly one
recv() for every 1024-byte chunk it sends, while the board acknowledges once
per read of up to 1460 bytes -- ArduinoOTA.cpp clamps to 1460, not to the
sender's chunk size.  As long as the board keeps up, a read covers one chunk
and the counts happen to match.  When the link stalls and data backs up in the
board's receive buffer, one read covers two chunks and emits one ack where
espota waits for two.  From then on espota is an ack behind, and at the end of
the file it blocks on a recv that will never come, times out after ten
seconds, and prints "Error Uploading" -- having delivered every byte.

Confirmed rather than guessed: all 1351 chunks of a 1.38 MB image were sent on
a run espota called a failure, and draining acks opportunistically instead of
demanding one per chunk let the same transfer over the same link complete and
reboot the board.

So this asks the board.  It records which commit is running, uploads, then
polls /api/v1/info until the commit changes to the one just built and uptime
resets.  Success means the device says so; nothing else counts, and espota's
own verdict is discarded.

Fixing it properly means either a corrected uploader here, or forking
ArduinoOTA to clamp its read to the sender's chunk size.  Neither is done; the
verification below makes the bug harmless in practice.

It also works out which network interface to send from, which on a machine
with VMware, WSL and VirtualBox installed is five wrong answers and one right
one, and is the other half of why OTA on Windows is a coin toss.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Optional

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SECRETS = os.path.join(ROOT, "include", "secrets.h")


def from_secrets(name: str) -> Optional[str]:
    """A #define out of the gitignored header, for local convenience."""
    try:
        src = open(SECRETS, encoding="utf-8").read()
    except OSError:
        return None
    m = re.search(r'#define\s+%s\s+"([^"]*)"' % name, src)
    return m.group(1) if m else None


def resolve(host: str) -> str:
    return socket.gethostbyname(host)


def local_address_for(target: str) -> str:
    """Which of our addresses the kernel would use to reach the board.

    No packet is sent -- connecting a UDP socket only picks a route -- but it
    is the routing table's own answer, which beats guessing among interfaces.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((target, 1))
        return str(s.getsockname()[0])
    finally:
        s.close()


def api(host: str, path: str, token: Optional[str], user: Optional[str],
        password: Optional[str], timeout: float = 6.0) -> Optional[dict]:
    req = urllib.request.Request("http://%s/api/v1%s" % (host, path))
    if token:
        req.add_header("Authorization", "Bearer " + token)
    opener: urllib.request.OpenerDirector
    if user and password:
        mgr = urllib.request.HTTPPasswordMgrWithDefaultRealm()
        mgr.add_password(None, "http://%s" % host, user, password)
        opener = urllib.request.build_opener(
            urllib.request.HTTPDigestAuthHandler(mgr))
    else:
        opener = urllib.request.build_opener()
    try:
        with opener.open(req, timeout=timeout) as r:
            return json.load(r)
    except Exception:
        return None


def git(*args: str) -> str:
    out = subprocess.run(["git"] + list(args), cwd=ROOT,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return out.stdout.decode("utf-8", "replace").strip()


def expected_commit() -> str:
    """What the firmware about to be built will report as its commit.

    Mirrors tools/git_rev.py, because the point is to compare like with like.
    """
    rev = git("rev-parse", "--short", "HEAD") or "unknown"
    return rev + ("+dirty" if git("status", "--porcelain") else "")


def build(pio: str, env: str) -> Optional[str]:
    print("building %s..." % env, end="", flush=True)
    r = subprocess.run([pio, "run", "-e", env], cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = r.stdout.decode("utf-8", "replace")
    if "SUCCESS" not in out:
        print(" failed")
        for line in out.splitlines():
            if "error" in line.lower():
                print("   " + line)
        return None
    build_dir = os.environ.get("PLATFORMIO_BUILD_DIR",
                               os.path.join(ROOT, ".pio", "build"))
    path = os.path.join(build_dir, env, "firmware.bin")
    if not os.path.exists(path):
        print(" built, but %s is missing" % path)
        return None
    print(" %d bytes" % os.path.getsize(path))
    return path


def upload(espota: str, python: str, ip: str, local: str, password: str,
           firmware: str) -> tuple[bool, str]:
    """Run espota once.  Its verdict is advisory; the caller checks the board."""
    cmd = [python, espota, "-i", ip, "-I", local, "-p", "3232",
           "-f", firmware]
    if password:
        cmd += ["-a", password]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=400)
    out = r.stdout.decode("utf-8", "replace")
    sent = out.count(".")
    claimed = "Result: OK" in out or "Success" in out
    return claimed, "%d blocks sent%s" % (
        sent, ", espota reported success" if claimed
        else ", espota reported failure")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="frank.local")
    ap.add_argument("--env", default="gray_rtc_ota")
    ap.add_argument("--password", help="OTA password; read from secrets.h if omitted")
    ap.add_argument("--token", help="bearer token for the API check")
    ap.add_argument("--user")
    ap.add_argument("--api-password")
    ap.add_argument("--retries", type=int, default=3)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--pio", default=os.path.expanduser(
        "~/.platformio/penv/Scripts/pio.exe"))
    args = ap.parse_args(argv[1:])

    ota_pw = args.password or from_secrets("OTA_PASSWORD") or ""
    token = args.token or from_secrets("AUTH_TOKEN_VALUE")
    user = args.user or from_secrets("AUTH_USER")
    api_pw = args.api_password or from_secrets("AUTH_PASS")

    try:
        ip = resolve(args.host)
    except Exception as e:
        print("cannot resolve %s: %s" % (args.host, e))
        print("(Windows has no mDNS resolver without Bonjour -- use the address)")
        return 1
    local = local_address_for(ip)
    print("%s is %s; sending from %s" % (args.host, ip, local))

    before = api(ip, "/info", token, user, api_pw)
    if before is None:
        print("the board is not answering /api/v1/info -- is it up, and are "
              "the credentials right?")
        return 1
    print("running:  %s (%s)" % (before.get("version"), before.get("commit")))

    want = expected_commit()
    print("uploading: %s" % want)
    if want == before.get("commit"):
        print("  note: the same commit, so a successful update looks identical")
        print("  from the outside.  Watching uptime instead.")

    firmware = None if args.no_build else build(args.pio, args.env)
    if not args.no_build and firmware is None:
        return 1
    if firmware is None:
        build_dir = os.environ.get("PLATFORMIO_BUILD_DIR",
                                   os.path.join(ROOT, ".pio", "build"))
        firmware = os.path.join(build_dir, args.env, "firmware.bin")

    espota = os.path.expanduser(
        "~/.platformio/packages/framework-arduinoespressif32/tools/espota.py")
    python = os.path.expanduser("~/.platformio/penv/Scripts/python.exe")

    uptime_before = 10 ** 9
    st = api(ip, "/state", token, user, api_pw)
    if st:
        uptime_before = st.get("system", {}).get("uptimeSeconds", 10 ** 9)

    for attempt in range(1, args.retries + 1):
        print("\nattempt %d of %d" % (attempt, args.retries))
        try:
            claimed, detail = upload(espota, python, ip, local, ota_pw, firmware)
        except subprocess.TimeoutExpired:
            claimed, detail = False, "espota did not finish"
        print("   %s" % detail)

        # The only verdict that counts: has the board come back on the new
        # firmware?  espota's own answer is wrong often enough to ignore.
        print("   asking the board...", end="", flush=True)
        deadline = time.time() + 90
        while time.time() < deadline:
            time.sleep(4)
            info = api(ip, "/info", token, user, api_pw, timeout=4)
            if not info:
                continue
            state = api(ip, "/state", token, user, api_pw, timeout=4)
            up = (state or {}).get("system", {}).get("uptimeSeconds", 10 ** 9)
            if info.get("commit") == want and up < uptime_before:
                print(" up %ss on %s" % (up, info.get("commit")))
                print("\nupdated.")
                return 0
        print(" no")
        if attempt < args.retries:
            print("   the board did not come back on %s; trying again" % want)
            time.sleep(10)

    print("\nthe update did not take after %d attempts." % args.retries)
    print("A transfer needs about 1350 round trips, so a weak link is the")
    print("usual reason -- check the signal with `curl .../api/v1/net`.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
