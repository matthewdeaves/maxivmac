#!/usr/bin/env python3
"""ethhub tests (stdlib only).  Run: python3 -I test_ethhub.py /path/to/ethhub

Build the hub with `make SAN=1` to run under ASan/UBSan: the tests check the
exit status, so a sanitizer report fails the run.
"""
import os
import random
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HUB = None


def mac(n):
    return bytes([0x02, 0, 0, 0, 0, n])


BCAST = b"\xff" * 6


def frame(dst, src, payload=b"x" * 46, etype=b"\x08\x00"):
    return dst + src + etype + payload


class Hub:
    def __init__(self, *args, listen="127.0.0.1:0"):
        self.dir = tempfile.TemporaryDirectory()
        self.errpath = os.path.join(self.dir.name, "stderr")
        self.err = open(self.errpath, "w+")
        self.p = subprocess.Popen([HUB, "--listen", listen, *args],
                                  stdout=subprocess.PIPE, stderr=self.err, text=True)
        line = self.p.stdout.readline()
        m = re.match(r"ethhub listening on (\d+)", line)
        if not m:
            raise RuntimeError("hub did not start: %r" % line)
        self.addr = ("127.0.0.1", int(m.group(1)))
        self.socks = []

    def client(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        s.settimeout(0.5)
        self.socks.append(s)
        return s

    def send(self, s, data):
        s.sendto(data, self.addr)

    def stats(self):
        """SIGUSR1 -> parse the newest 'ethhub stats' line."""
        def lines():
            self.err.flush()
            with open(self.errpath) as f:
                return [l for l in f if l.startswith("ethhub stats")]
        n = len(lines())
        self.p.send_signal(signal.SIGUSR1)
        for _ in range(100):
            ls = lines()
            if len(ls) > n:
                return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", ls[-1])}
            time.sleep(0.05)
        raise RuntimeError("no stats")

    def stop(self):
        for s in self.socks:
            s.close()
        self.p.send_signal(signal.SIGTERM)
        rc = self.p.wait(timeout=10)
        self.err.seek(0)
        text = self.err.read()
        self.err.close()
        self.p.stdout.close()
        self.dir.cleanup()
        return rc, text


def recv(s, t=0.5):
    s.settimeout(t)
    try:
        return s.recvfrom(65535)[0]
    except socket.timeout:
        return None


def drain(*socks):
    for s in socks:
        s.setblocking(False)
        try:
            while True:
                s.recv(65535)
        except (BlockingIOError, OSError):
            pass


def settle(*socks):
    """Wait until the hub has processed everything sent so far (loopback FIFO)."""
    time.sleep(0.15)


class HubTest(unittest.TestCase):
    def start(self, *args):
        self.hub = Hub(*args)
        self.addCleanup(self.finish)
        return self.hub

    def finish(self):
        if self.hub:
            rc, text = self.hub.stop()
            self.hub = None
            self.assertEqual(rc, 0, text)
            self.assertNotIn("Sanitizer", text)
            self.assertNotIn("runtime error", text)

    hub = None

    def three(self, *args):
        h = self.start(*args)
        a, b, c = h.client(), h.client(), h.client()
        for s in (a, b, c):
            h.send(s, b"")
        settle()
        return h, a, b, c

    def test_hello_registers_without_traffic(self):
        h, a, b, c = self.three()
        st = h.stats()
        self.assertEqual(st["clients"], 3)
        self.assertEqual(st["hellos"], 3)
        self.assertEqual(st["rx_frames"], 0)
        # a registered-but-silent client receives floods
        h.send(a, frame(BCAST, mac(1)))
        self.assertIsNotNone(recv(b))
        self.assertIsNotNone(recv(c))

    def test_broadcast_floods_not_to_sender(self):
        h, a, b, c = self.three()
        f = frame(BCAST, mac(1))
        h.send(a, f)
        self.assertEqual(recv(b), f)
        self.assertEqual(recv(c), f)
        self.assertIsNone(recv(a, 0.3))

    def test_multicast_floods(self):
        h, a, b, c = self.three()
        f = frame(b"\x01\x00\x5e\x00\x00\x01", mac(1))
        h.send(a, f)
        self.assertEqual(recv(b), f)
        self.assertEqual(recv(c), f)
        self.assertIsNone(recv(a, 0.3))

    def test_unicast_to_learned_mac(self):
        h, a, b, c = self.three()
        h.send(b, frame(BCAST, mac(2)))  # teach hub: mac2 is behind b
        recv(a), recv(c)
        f = frame(mac(2), mac(1))
        h.send(a, f)
        self.assertEqual(recv(b), f)
        self.assertIsNone(recv(c, 0.3))
        self.assertIsNone(recv(a, 0.3))
        self.assertEqual(h.stats()["unicast"], 1)

    def test_unknown_unicast_floods(self):
        h, a, b, c = self.three()
        f = frame(mac(9), mac(1))
        h.send(a, f)
        self.assertEqual(recv(b), f)
        self.assertEqual(recv(c), f)
        self.assertIsNone(recv(a, 0.3))
        self.assertEqual(h.stats()["unknown_unicast"], 1)

    def test_mac_moves_between_clients(self):
        h, a, b, c = self.three()
        h.send(b, frame(BCAST, mac(2)))
        recv(a), recv(c)
        h.send(c, frame(BCAST, mac(2)))  # mac2 moved to c
        recv(a), recv(b)
        f = frame(mac(2), mac(1))
        h.send(a, f)
        self.assertEqual(recv(c), f)
        self.assertIsNone(recv(b, 0.3))

    def test_dst_on_same_port_not_echoed(self):
        h, a, b, c = self.three()
        h.send(a, frame(BCAST, mac(1)))
        recv(b), recv(c)
        h.send(a, frame(mac(1), mac(7)))  # dst learned on the sender's own port
        self.assertIsNone(recv(a, 0.3))
        self.assertIsNone(recv(b, 0.3))

    def test_size_limits(self):
        h, a, b, c = self.three()
        for n in (1, 5, 13):
            h.send(a, b"\xff" * n)
        h.send(a, frame(BCAST, mac(1), b"y" * (1515 - 14)))  # 1515: too long
        h.send(a, b"\xff" * 9000)
        self.assertIsNone(recv(b, 0.4))
        # boundaries are accepted
        small = frame(BCAST, mac(1), b"")  # 14
        big = frame(BCAST, mac(1), b"z" * (1514 - 14))
        h.send(a, small)
        h.send(a, big)
        self.assertEqual(recv(b), small)
        self.assertEqual(recv(b), big)
        st = h.stats()
        self.assertEqual(st["dropped_short"], 3)
        self.assertEqual(st["dropped_long"], 2)

    def test_client_ageing(self):
        h, a, b, c = self.three("--client-timeout", "2")
        t0 = time.time()
        while time.time() - t0 < 3.6:  # keep a alive, let b and c expire
            h.send(a, b"")
            time.sleep(0.4)
        st = h.stats()
        self.assertEqual(st["clients"], 1)
        self.assertEqual(st["clients_expired"], 2)
        h.send(c, b"")  # c returns
        settle()
        f = frame(BCAST, mac(1))
        h.send(a, f)
        self.assertEqual(recv(c), f)
        self.assertIsNone(recv(b, 0.3))

    def test_mac_ageing(self):
        h, a, b, c = self.three("--mac-timeout", "2")
        h.send(b, frame(BCAST, mac(2)))
        recv(a), recv(c)
        f = frame(mac(2), mac(1))
        h.send(a, f)
        self.assertEqual(recv(b), f)
        self.assertIsNone(recv(c, 0.3))
        time.sleep(3.5)
        h.send(a, f)  # mac2 forgotten: floods
        self.assertEqual(recv(b), f)
        self.assertEqual(recv(c), f)
        self.assertGreaterEqual(h.stats()["macs_expired"], 1)

    def test_client_table_bound(self):
        h = self.start("--max-clients", "2")
        a, b, c = h.client(), h.client(), h.client()
        h.send(a, b"")
        h.send(b, b"")
        h.send(c, b"")
        settle()
        h.send(c, frame(BCAST, mac(3)))  # unregistered: ignored
        f = frame(BCAST, mac(1))
        h.send(a, f)
        self.assertEqual(recv(b), f)
        self.assertIsNone(recv(c, 0.3))
        self.assertIsNone(recv(a, 0.3))
        st = h.stats()
        self.assertEqual(st["clients"], 2)
        self.assertEqual(st["dropped_full"], 2)

    def test_mac_table_bound(self):
        h = self.start("--max-macs", "3")
        a, b = h.client(), h.client()
        h.send(a, b"")
        h.send(b, b"")
        for i in range(10, 20):
            h.send(a, frame(BCAST, mac(i)))
            recv(b)
        st = h.stats()
        self.assertEqual(st["macs"], 3)
        self.assertEqual(st["macs_evicted"], 7)
        # newest learned entry still unicasts
        f = frame(mac(19), mac(1))
        h.send(b, f)
        self.assertEqual(recv(a), f)

    def test_pcap(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        path = os.path.join(d.name, "x.pcap")
        h = self.start("--pcap", path)
        a, b = h.client(), h.client()
        h.send(a, b"")
        h.send(b, b"")
        f1, f2 = frame(BCAST, mac(1)), frame(mac(1), mac(2), b"q" * 100)
        h.send(a, f1)
        recv(b)
        h.send(b, f2)
        recv(a)
        h.stats()
        with open(path, "rb") as fh:
            data = fh.read()
        magic, vmaj, vmin, _, _, snap, link = struct.unpack("<IHHiIII", data[:24])
        self.assertEqual((magic, vmaj, vmin, link), (0xA1B2C3D4, 2, 4, 1))
        off, got = 24, []
        while off < len(data):
            _, _, caplen, origlen = struct.unpack("<IIII", data[off:off + 16])
            self.assertEqual(caplen, origlen)
            got.append(data[off + 16:off + 16 + caplen])
            off += 16 + caplen
        self.assertEqual(got, [f1, f2])

    def test_fuzz_does_not_crash(self):
        h, a, b, c = self.three("--max-clients", "8", "--max-macs", "16", "--verbose")
        rng = random.Random(12345)
        socks = [h.client() for _ in range(6)]
        for s in socks:
            s.setblocking(False)
        sent = 0
        for i in range(6000):
            r = rng.random()
            if r < 0.1:
                n = rng.randrange(0, 20)
            elif r < 0.15:
                n = rng.randrange(1500, 9000)
            else:
                n = rng.randrange(14, 1515)
            d = bytearray(rng.randbytes(n))
            if n >= 14 and rng.random() < 0.5:
                d[0:6] = rng.choice([BCAST, mac(rng.randrange(8)), b"\x01\x00\x5e" + d[3:6]])
                d[6:12] = mac(rng.randrange(1, 12))
            try:
                rng.choice(socks).sendto(bytes(d), h.addr)
                sent += 1
            except (BlockingIOError, OSError):
                pass
            if i % 200 == 0:  # drain so socket buffers don't clog the test
                drain(*socks, a, b, c)
                time.sleep(0.02)
        self.assertGreater(sent, 5000)
        # still alive and sane
        time.sleep(0.2)
        drain(a, b, c)
        f = frame(BCAST, mac(1))
        h.send(a, f)
        got = None
        deadline = time.time() + 15
        while time.time() < deadline:  # skip any fuzz backlog still draining
            got = recv(b, 0.3)
            if got == f:
                break
        self.assertEqual(got, f)
        st = h.stats()
        self.assertLessEqual(st["clients"], 8)
        self.assertLessEqual(st["macs"], 16)

    def test_uplink_joins_other_hub(self):
        h1 = self.start()
        a = h1.client()
        h1.send(a, b"")
        h2 = Hub("--connect", "127.0.0.1:%d" % h1.addr[1], "--client-timeout", "2")
        self.addCleanup(lambda: self.assertEqual(h2.stop()[0], 0))
        b = h2.client()
        h2.send(b, b"")
        time.sleep(1.0)  # uplink hello registers h2 at h1
        f = frame(BCAST, mac(1))
        h1.send(a, f)
        self.assertEqual(recv(b), f)
        g = frame(mac(1), mac(2))  # reply is learned through the uplink: unicast
        h2.send(b, g)
        self.assertEqual(recv(a), g)
        # the pinned uplink survives longer than h2's client timeout
        time.sleep(3.5)
        h2.send(b, b"")
        settle()
        h1.send(a, f)
        self.assertEqual(recv(b), f)

    def test_bad_args(self):
        for args in (["--listen", "x:99999"], ["--max-clients", "0"], ["--bogus"]):
            r = subprocess.run([HUB, *args], capture_output=True, timeout=10)
            self.assertEqual(r.returncode, 2, args)


if __name__ == "__main__":
    HUB = os.path.abspath(sys.argv.pop(1))
    unittest.main(verbosity=2)
