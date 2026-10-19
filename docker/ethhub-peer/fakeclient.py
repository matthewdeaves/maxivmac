#!/usr/bin/env python3
"""Host-side fake ethhub client: speaks ARP/ICMP/UDP over the hub's UDP framing.

  fakeclient.py [--hub 127.0.0.1:5555] [--ip 10.88.0.30] [--mac 02:00:00:00:00:20] CMD ...
    arp TARGET_IP               who-has TARGET_IP, print the reply
    ping TARGET_IP              ARP + one ICMP echo, print the reply
    bcast PORT TEXT [ADDR]      UDP broadcast (default 10.88.0.255); answers ARP and prints replies
"""
import argparse
import socket
import struct
import time


def mac_b(s):
    return bytes(int(x, 16) for x in s.split(":"))


def mac_s(b):
    return ":".join("%02x" % x for x in b)


def cksum(d):
    if len(d) % 2:
        d += b"\0"
    s = sum(struct.unpack("!%dH" % (len(d) // 2), d))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def arp(op, smac, sip, tmac, tip):
    return struct.pack("!HHBBH", 1, 0x0800, 6, 4, op) + smac + sip + tmac + tip


def ip_pkt(proto, src, dst, payload):
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0x1234, 0, 64, proto, 0, src, dst)
    h = h[:10] + struct.pack("!H", cksum(h)) + h[12:]
    return h + payload


class Client:
    def __init__(self, hub, ip, mac):
        host, port = hub.rsplit(":", 1)
        self.hub = (host, int(port))
        self.ip, self.mac = socket.inet_aton(ip), mac_b(mac)
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.sendto(b"", self.hub)  # hello

    def send(self, dst, etype, payload):
        f = dst + self.mac + struct.pack("!H", etype) + payload
        self.s.sendto(f.ljust(60, b"\0"), self.hub)

    def recv(self, timeout):
        """Next frame; answers ARP requests for our IP transparently."""
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                return None
            self.s.settimeout(left)
            try:
                f = self.s.recv(2048)
            except socket.timeout:
                return None
            if len(f) >= 42 and f[12:14] == b"\x08\x06":
                op = struct.unpack("!H", f[20:22])[0]
                if op == 1 and f[38:42] == self.ip:
                    print("  [answered ARP who-has from %s]" % socket.inet_ntoa(f[28:32]))
                    self.send(f[6:12], 0x0806, arp(2, self.mac, self.ip, f[22:28], f[28:32]))
                    continue
            return f

    def resolve(self, tip):
        t = socket.inet_aton(tip)
        self.send(b"\xff" * 6, 0x0806, arp(1, self.mac, self.ip, b"\0" * 6, t))
        end = time.time() + 3
        while time.time() < end:
            f = self.recv(end - time.time())
            if f and f[12:14] == b"\x08\x06" and struct.unpack("!H", f[20:22])[0] == 2 and f[28:32] == t:
                return f[22:28]
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hub", default="127.0.0.1:5555")
    ap.add_argument("--ip", default="10.88.0.30")
    ap.add_argument("--mac", default="02:00:00:00:00:20")
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    c = Client(a.hub, a.ip, a.mac)
    if a.cmd == "arp":
        m = c.resolve(a.args[0])
        print("ARP reply: %s is-at %s" % (a.args[0], mac_s(m)) if m else "NO ARP reply")
        raise SystemExit(0 if m else 1)
    if a.cmd == "ping":
        tip = a.args[0]
        m = c.resolve(tip)
        if not m:
            print("NO ARP reply")
            raise SystemExit(1)
        print("ARP reply: %s is-at %s" % (tip, mac_s(m)))
        icmp = struct.pack("!BBHHH", 8, 0, 0, 0x4242, 1) + b"ethhub-ping-payload"
        icmp = icmp[:2] + struct.pack("!H", cksum(icmp)) + icmp[4:]
        c.send(m, 0x0800, ip_pkt(1, c.ip, socket.inet_aton(tip), icmp))
        t0 = time.time()
        end = t0 + 3
        while time.time() < end:
            f = c.recv(end - time.time())
            if f and f[12:14] == b"\x08\x00" and f[23] == 1 and f[34] == 0:
                print("ICMP echo reply from %s, %d bytes, %.1f ms"
                      % (socket.inet_ntoa(f[26:30]), len(f), (time.time() - t0) * 1000))
                return
        print("NO ICMP echo reply")
        raise SystemExit(1)
    if a.cmd == "bcast":
        port, text = int(a.args[0]), a.args[1].encode()
        dst = a.args[2] if len(a.args) > 2 else "10.88.0.255"
        udp = struct.pack("!HHHH", 4000, port, 8 + len(text), 0) + text
        c.send(b"\xff" * 6, 0x0800, ip_pkt(17, c.ip, socket.inet_aton(dst), udp))
        end = time.time() + 3
        while time.time() < end:
            f = c.recv(end - time.time())
            if f and f[12:14] == b"\x08\x00" and f[23] == 17 and struct.unpack("!H", f[36:38])[0] == 4000:
                print("UDP reply from %s:%d -> %r" % (socket.inet_ntoa(f[26:30]),
                      struct.unpack("!H", f[34:36])[0], f[42:34 + struct.unpack("!H", f[38:40])[0]]))
                return
        print("NO UDP reply")
        raise SystemExit(1)
    raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
