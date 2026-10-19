#!/usr/bin/env python3
"""ctest driver: start the real ethhub on an ephemeral loopback port and run
eth-hub-it (three EthHub backends) against it.

  python3 -I eth_hub_it.py /path/to/ethhub /path/to/eth-hub-it
"""
import re
import signal
import subprocess
import sys


def main():
    hub_bin, it_bin = sys.argv[1], sys.argv[2]
    hub = subprocess.Popen([hub_bin, "--listen", "127.0.0.1:0"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        line = hub.stdout.readline()
        m = re.match(r"ethhub listening on (\d+)", line)
        if not m:
            print("hub did not start: %r" % line)
            return 1
        r = subprocess.run([it_bin, "127.0.0.1:%s" % m.group(1)], timeout=60)
        return r.returncode
    finally:
        hub.send_signal(signal.SIGTERM)
        try:
            hub.wait(timeout=5)
        except subprocess.TimeoutExpired:
            hub.kill()


if __name__ == "__main__":
    sys.exit(main())
