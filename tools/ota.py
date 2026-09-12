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

So the upload is implemented here instead, correctly: the image is streamed
and acknowledgements are drained as they arrive, without requiring any
particular number of them.  TCP already supplies the backpressure espota was
trying to impose by counting.  See the comments above push().

It then asks the board anyway.  After uploading it polls /api/v1/info until
the commit changes to the one just built and uptime resets -- because an
uploader saying "done" and a device running the new firmware are different
claims, and only the second one is the one anybody wants.

It also works out which network interface to send from, which on a machine
with VMware, WSL and VirtualBox installed is five wrong answers and one right
one, and is the other half of why OTA on Windows is a coin toss.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import select
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


# --------------------------------------------------------------- the upload --
#
# The ESP OTA protocol, implemented here rather than shelled out to espota.py.
#
# The protocol itself is simple and fine.  We invite the board over UDP, it
# connects back to us over TCP, we send the image, it writes it to flash and
# answers "OK".  What espota gets wrong is the bookkeeping in the middle: it
# performs exactly one recv() for every 1024-byte chunk it sends, while the
# board acknowledges once per read of up to 1460 bytes.  Those counts match
# only while the board keeps up.  As soon as the link stalls and data backs up
# in the board's receive buffer, one read covers two chunks and answers once --
# and espota spends the rest of the file an acknowledgement behind, ending on
# a recv that never comes.
#
# The fix is to stop counting.  The acknowledgements are advisory: ArduinoOTA
# sends them so a stalled sender has something to react to, and does not
# require the sender to wait for them.  TCP already provides the backpressure
# that matters -- when the board cannot keep up its receive window closes and
# our send blocks, which is precisely the pacing espota was trying to impose
# by hand.  So we stream the image and drain acknowledgements as they arrive,
# never requiring a particular number of them, and read for "OK" at the end.
#
# Draining is not optional, though.  If we only ever wrote, the board's
# acknowledgements would fill our receive buffer, its printf() would fail, and
# ArduinoOTA treats a failed printf as a dead connection and gives up.  Hence
# select() on both directions rather than a simple sendall().

FLASH = 0
AUTH = 200
SEND_CHUNK = 1460  # what the board reads at once; keeps progress smooth


class UploadError(Exception):
    """The upload genuinely failed, as opposed to espota thinking it did."""


def md5hex(s: str) -> str:
    return hashlib.md5(s.encode("utf-8")).hexdigest()


def invite(udp: socket.socket, addr: tuple, local_port: int, size: int,
           digest: str, password: str, name: str) -> None:
    """Tell the board an update is coming and where to collect it.

    The board answers "OK", or "AUTH <nonce>" if it wants a password.  Note
    that it connects back to whatever address our UDP packet came *from*, not
    to anything named in the message -- which is why the caller binds this
    socket rather than letting the routing table choose.  espota leaves that
    to chance and only binds its listener, which is half the reason OTA is a
    coin toss on a machine with several interfaces.
    """
    message = ("%d %d %d %s\n" % (FLASH, local_port, size, digest)).encode()
    answer = ""
    for _ in range(10):
        udp.sendto(message, addr)
        udp.settimeout(3.0)
        try:
            answer = udp.recv(37).decode("utf-8", "replace")
            break
        except socket.timeout:
            continue
    else:
        raise UploadError("the board never answered the invitation")

    if answer == "OK":
        return
    if not answer.startswith("AUTH"):
        raise UploadError("unexpected answer to the invitation: %r" % answer)

    # Digest challenge.  The cnonce is ours to choose -- the board only folds
    # it into the hash -- but it is derived the way espota derives it so that
    # anything watching sees a familiar exchange.
    nonce = answer.split()[1]
    cnonce = md5hex("%s%u%s%s" % (name, size, digest, addr[0]))
    result = md5hex("%s:%s:%s" % (md5hex(password), nonce, cnonce))
    udp.sendto(("%d %s %s\n" % (AUTH, cnonce, result)).encode(), addr)
    udp.settimeout(10.0)
    try:
        answer = udp.recv(32).decode("utf-8", "replace")
    except socket.timeout:
        raise UploadError("no answer to the authentication")
    if answer != "OK":
        raise UploadError("authentication refused (%s) -- wrong OTA password?"
                          % answer.strip())


def stream(conn: socket.socket, blob: bytes, stall_s: float = 30.0) -> bytes:
    """Send the image, draining acknowledgements as they turn up.

    Returns whatever the board said along the way.  Raises only when the
    transfer genuinely stops: the connection closing early, or nothing moving
    in either direction for stall_s.
    """
    conn.setblocking(False)
    total = len(blob)
    sent = 0
    heard = bytearray()
    last_move = time.time()
    shown = -1
    tty = sys.stdout.isatty()

    while sent < total:
        readable, writable, _ = select.select([conn], [conn], [], 1.0)

        if readable:
            chunk = conn.recv(4096)
            if not chunk:
                raise UploadError(
                    "the board hung up after %d of %d bytes" % (sent, total))
            heard += chunk
            last_move = time.time()

        if writable:
            try:
                n = conn.send(blob[sent:sent + SEND_CHUNK])
            except BlockingIOError:
                # select() and send() can disagree about a socket that filled
                # in between them.  Not an error; go round again.
                n = 0
            if n:
                sent += n
                last_move = time.time()
                pct = sent * 100 // total
                if tty and pct != shown:
                    shown = pct
                    print("\r   uploading %3d%%" % pct, end="", flush=True)
                elif not tty and pct // 25 != shown // 25:
                    shown = pct
                    print("   uploading %d%%" % (pct // 25 * 25), flush=True)

        if time.time() - last_move > stall_s:
            raise UploadError("stalled for %.0fs with %d of %d bytes sent"
                              % (stall_s, sent, total))

    print(("\r" if tty else "") + "   uploaded %d bytes   " % total)
    return bytes(heard)


def await_ok(conn: socket.socket, heard: bytes, timeout: float = 60.0) -> None:
    """Wait for the board to finish writing flash and say so.

    Everything it has sent is acknowledgement counts -- bare decimal numbers,
    no framing -- until the last word, which is "OK" once Update.end() has
    verified the image.  Anything else in there is an error message.
    """
    tail = bytearray(heard)
    conn.setblocking(True)
    deadline = time.time() + timeout
    while b"OK" not in tail and time.time() < deadline:
        conn.settimeout(max(1.0, deadline - time.time()))
        try:
            chunk = conn.recv(256)
        except socket.timeout:
            break
        if not chunk:
            break
        tail += chunk

    if b"OK" in tail:
        return

    # Strip the acknowledgement digits; whatever is left is the complaint.
    complaint = re.sub(rb"[0-9]+", b"", bytes(tail)).strip()
    if complaint:
        raise UploadError("the board rejected the image: %s"
                          % complaint.decode("utf-8", "replace"))
    raise UploadError("the board never confirmed the image "
                      "(it may still have applied it -- the check below will "
                      "say)")


def push(ip: str, local: str, port: int, password: str,
         firmware: str) -> tuple[bool, str]:
    """Upload one image.  True means the board confirmed it."""
    blob = open(firmware, "rb").read()
    digest = hashlib.md5(blob).hexdigest()

    # Listen before inviting, so there is no window in which the board
    # connects back to a socket that does not exist yet.
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    conn = None
    try:
        srv.bind((local, 0))
        srv.listen(1)
        udp.bind((local, 0))

        invite(udp, (ip, port), srv.getsockname()[1], len(blob), digest,
               password, os.path.basename(firmware))

        srv.settimeout(30.0)
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            raise UploadError(
                "the board accepted the invitation but never connected back "
                "to %s -- a firewall, or the wrong interface" % local)

        heard = stream(conn, blob)
        await_ok(conn, heard)
        return True, "the board confirmed the image"
    except UploadError as e:
        return False, str(e)
    except OSError as e:
        # A reset, a refused connection, an interface that went away.  The
        # caller retries, so report it rather than ending the run.
        return False, "the connection failed: %s" % e
    finally:
        if conn:
            conn.close()
        srv.close()
        udp.close()


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

    # Give a board that is still coming up a chance.  Rejoining WiFi after a
    # reboot takes twelve to fifteen seconds, which is exactly the window you
    # land in when updating twice in a row.
    before = api(ip, "/info", token, user, api_pw)
    if before is None:
        print("waiting for the board...", end="", flush=True)
        deadline = time.time() + 60
        while before is None and time.time() < deadline:
            time.sleep(3)
            before = api(ip, "/info", token, user, api_pw, timeout=4)
        print(" here" if before else " no")
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

    if args.no_build:
        print("  note: --no-build, so the image on disk may predate the")
        print("  working tree.  The check below compares the board against")
        print("  what HEAD says now, and will disagree if it does.")
    firmware = None if args.no_build else build(args.pio, args.env)
    if not args.no_build and firmware is None:
        return 1
    if firmware is None:
        build_dir = os.environ.get("PLATFORMIO_BUILD_DIR",
                                   os.path.join(ROOT, ".pio", "build"))
        firmware = os.path.join(build_dir, args.env, "firmware.bin")

    # How to tell the board rebooted.
    #
    # Comparing its uptime against the uptime before the upload is not enough,
    # and fails in the one case worth testing: upload twice in a row and the
    # "before" reading is itself a board that rebooted a moment ago, so the
    # new uptime has to beat about fifteen seconds and whether it does is
    # luck.  What identifies a reboot is not a small uptime but an uptime
    # smaller than the board would have had if it had stayed up -- which is
    # the earlier reading plus however long we have taken since.
    uptime_before = 10 ** 9
    measured_at = time.time()
    st = api(ip, "/state", token, user, api_pw)
    if st:
        uptime_before = st.get("system", {}).get("uptimeSeconds", 10 ** 9)

    def rebooted(up: float) -> bool:
        if uptime_before >= 10 ** 9:
            return True  # never got a reading, so do not hold it against it
        alive = uptime_before + (time.time() - measured_at)
        return up < alive - 15

    for attempt in range(1, args.retries + 1):
        print("\nattempt %d of %d" % (attempt, args.retries))
        claimed, detail = push(ip, local, 3232, ota_pw, firmware)
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
            if info.get("commit") != want:
                continue
            # A commit that changed is proof on its own.  Only when the new
            # image carries the same commit as the old one -- reflashing the
            # same build -- does the reboot have to carry the argument.
            if before.get("commit") != want or rebooted(up):
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
