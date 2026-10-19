#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Part of maxivmac (GPL v2, see LICENSE).
"""Tiny LAN test peer.

  netecho.py serve PORT                 UDP listener (sees broadcasts) + TCP echo server
  netecho.py bcast ADDR PORT TEXT       send one UDP datagram (use a broadcast ADDR)
  netecho.py tcp HOST PORT TEXT         connect, send TEXT, print the echoed reply
"""
import socket
import sys
import threading


def serve(port):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    u.bind(("0.0.0.0", port))
    t = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    t.bind(("0.0.0.0", port))
    t.listen(8)

    def udp_loop():
        while True:
            data, addr = u.recvfrom(2048)
            print("UDP from %s:%d: %r" % (addr[0], addr[1], data), flush=True)
            u.sendto(b"ack:" + data, addr)  # unicast reply

    def tcp_conn(c, addr):
        print("TCP connect from %s:%d" % addr, flush=True)
        with c:
            while True:
                d = c.recv(4096)
                if not d:
                    break
                print("TCP from %s:%d: %r" % (addr[0], addr[1], d), flush=True)
                c.sendall(d)

    threading.Thread(target=udp_loop, daemon=True).start()
    print("netecho serving on %d (udp+tcp)" % port, flush=True)
    while True:
        c, a = t.accept()
        threading.Thread(target=tcp_conn, args=(c, a), daemon=True).start()


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "serve":
        serve(int(a[1]))
    elif len(a) == 4 and a[0] == "bcast":
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.settimeout(3)
        s.sendto(a[3].encode(), (a[1], int(a[2])))
        try:
            print("reply:", s.recvfrom(2048))
        except socket.timeout:
            print("no reply")
    elif len(a) == 4 and a[0] == "tcp":
        s = socket.create_connection((a[1], int(a[2])), timeout=5)
        s.sendall(a[3].encode())
        print("echo:", s.recv(4096).decode())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
