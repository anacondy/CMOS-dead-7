#!/usr/bin/env python3
"""Minimal SNTP server for TimeKeeper's acceptance tests — not a product file.

Listens on UDP/123 (run as root, or in a user+net namespace) and answers a
48-byte client request with a valid mode-4 reply whose timestamps are derived
from a *controllable* clock, so the test can simulate:

  * a normal, correct server            (default)
  * a server that is one month ahead    (--shift 2678400)  -> .dat bump
  * a bogus, ancient server             (--shift -5000000000) -> must be rejected
  * a kiss-o'-death packet              (--kod)
  * a silent black hole                 (--drop)
  * a reflected copy of the request     (--reflect) -> must be rejected as LOOPED

Usage:
  sudo python3 tests/sntp_responder.py --port 123 [--shift SECS] [--once] [--verbose]

SPDX-License-Identifier: MIT
"""
import argparse
import socket
import struct
import sys
import time

NTP_UNIX_DELTA = 2208988800
FLAGS_SERVER = (0 << 6) | (4 << 3) | 4          # LI=0, VN=4, mode=4 (server)


def to_ntp(ts: float) -> bytes:
    sec = int(ts) + NTP_UNIX_DELTA
    frac = int((ts % 1.0) * (1 << 32)) & 0xFFFFFFFF
    return struct.pack("!II", sec & 0xFFFFFFFF, frac)


def build_reply(req: bytes, mode: int, stratum: int, shift: float, reflect: bool,
                epoch=None) -> bytes:
    now = (time.time() + shift) if epoch is None else epoch
    pkt = bytearray(48)
    pkt[0] = (0 << 6) | (4 << 3) | mode
    pkt[1] = stratum
    pkt[2] = 4                                  # poll interval (log2 s)
    pkt[3] = 0xEC                                # precision ~ -20 (about 1 us)
    pkt[4:8] = struct.pack("!i", 0)              # root delay: 0
    pkt[8:12] = struct.pack("!I", 0x00050000)    # root dispersion: 5 ms
    pkt[12:16] = b"LOCL"                         # reference identifier
    pkt[16:24] = to_ntp(now)                     # reference timestamp
    pkt[24:32] = req[40:48]                       # originate = client's transmit
    pkt[32:40] = to_ntp(now - 0.001)             # receive timestamp
    if reflect:
        pkt[40:48] = req[40:48]                  # echo the request -> LOOPED
    else:
        pkt[40:48] = to_ntp(now)
    return bytes(pkt)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=123)
    ap.add_argument("--addr", default="0.0.0.0")
    ap.add_argument("--shift", type=float, default=0.0,
                    help="seconds to offset the served time from the LOCAL clock")
    ap.add_argument("--epoch", type=float, default=None,
                    help="serve this absolute unix epoch instead of local clock + shift. "
                         "Use it whenever the test has already moved the system clock: "
                         "``now + shift`` would otherwise serve time derived from the "
                         "doctored clock and the test would be self-referential.")
    ap.add_argument("--mode", type=int, default=4)
    ap.add_argument("--stratum", type=int, default=2)
    ap.add_argument("--kod", action="store_true", help="answer Kiss-o'-Death (stratum 0)")
    ap.add_argument("--reflect", action="store_true", help="echo the client's transmit timestamp")
    ap.add_argument("--drop", action="store_true", help="never answer (black hole)")
    ap.add_argument("--once", action="store_true", help="serve one request then exit")
    ap.add_argument("--max", type=int, default=0, help="exit after N requests")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind((a.addr, a.port))
    except PermissionError:
        print(f"cannot bind :{a.port} - run as root", file=sys.stderr)
        return 2
    print(f"sntp-responder: listening on {a.addr}:{a.port} shift={a.shift} "
          f"mode={a.mode} stratum={a.stratum} kod={a.kod} reflect={a.reflect} drop={a.drop}",
          flush=True)

    n = 0
    while True:
        data, addr = s.recvfrom(1024)
        n += 1
        if a.verbose:
            print(f"  request {len(data)} bytes from {addr}", flush=True)
        if len(data) < 48:
            continue
        if a.drop:
            if a.verbose:
                print("  dropping (black hole)", flush=True)
        else:
            stratum = 0 if a.kod else a.stratum
            mode = 5 if a.kod else a.mode
            reply = build_reply(data, mode, stratum, a.shift, a.reflect, a.epoch)
            if a.kod:
                reply = bytearray(reply)
                reply[12:16] = b"RATE"           # kiss code in ref id
                reply = bytes(reply)
            s.sendto(reply, addr)
        if a.once or (a.max and n >= a.max):
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
