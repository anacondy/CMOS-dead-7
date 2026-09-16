#!/usr/bin/env python3
"""HTTP fixture for TimeKeeper's tier-2 (Date header) acceptance tests.

Speaks just enough HTTP/1.1 to be worth testing against: a real status line, a
real Date header, Connection: close. Modes let the suite prove that the client
refuses the things it must refuse -- a redirect, a captive-portal 200 with no
Date, a slow response, and a wrong weekday.

Usage
  python3 tests/http_date_server.py --port 8080 [--mode ok|redirect|nodoc|bogus|hang|utc]
                                    [--epoch N] [--requests 4]

--mode ok        200 + Date: <now>                     (must be accepted)
--mode utc       200 + Date for --epoch (absolute)     (must yield that instant)
--mode redirect  302 + Date                            (raw path must refuse)
--mode nodoc     200 + body, no Date header            (must be refused)
--mode bogus     200 + Date: Tue, 31 Feb 2031 00:00:00 GMT   (must be refused)
--mode hang      accepts, sends nothing                (timeout must bound it)

SPDX-License-Identifier: MIT
"""
import argparse
import email.utils
import socket
import sys
import time


def date_line(epoch):
    return email.utils.formatdate(epoch, usegmt=True)


def handle(conn, mode, epoch):
    try:
        conn.settimeout(3)
        try:
            conn.recv(4096)
        except Exception:
            pass
        if mode == "hang":
            time.sleep(30)          # client must give up on its own
            return
        now = epoch if epoch else int(time.time())
        if mode == "redirect":
            head = "HTTP/1.1 302 Found\r\nLocation: http://example.invalid/\r\n"
        else:
            head = "HTTP/1.1 200 OK\r\n"
        body = "<html><body>hi</body></html>\r\n"
        hdrs = "Server: tk-fixture\r\nConnection: close\r\n"
        if mode != "nodoc":
            d = "Tue, 31 Feb 2031 00:00:00 GMT" if mode == "bogus" else date_line(now)
            hdrs += "Date: %s\r\n" % d
        if mode == "redirect":
            hdrs += "Content-Length: 0\r\n\r\n"
        else:
            hdrs += "Content-Length: %d\r\n\r\n" % len(body)
        conn.sendall((head + hdrs + (body if mode != "redirect" else "")).encode("ascii"))
    finally:
        try:
            conn.shutdown(socket.SHUT_WR)
        except Exception:
            pass
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--addr", default="127.0.0.1")
    ap.add_argument("--mode", default="ok",
                    choices=["ok", "utc", "redirect", "nodoc", "bogus", "hang"])
    ap.add_argument("--epoch", type=int, default=None)
    ap.add_argument("--requests", type=int, default=4)
    a = ap.parse_args()

    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind((a.addr, a.port))
    except OSError as e:
        print("cannot bind :%d (%s)" % (a.port, e), file=sys.stderr)
        return 2
    s.listen(8)
    print("http-fixture: listening on %s:%d mode=%s" % (a.addr, a.port, a.mode), flush=True)
    for _ in range(a.requests):
        try:
            c, _ = s.accept()
        except Exception:
            break
        handle(c, a.mode, a.epoch)
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
