#!/usr/bin/env python3
"""Shows what a NETLOG=1 test build broadcasts: the payload's log (with curl's
details) and, on the PS4, the home screen app's report. Run it on any machine
on the console's network, then start the payload or open RommPS.

    python3 tools/netlog.py [--port 8781] [--out netlog.txt]

Everything is also appended to the --out file, for sending along. If nothing
shows up, the network drops broadcasts: put this machine's IP address in
/data/romm-sync/netlog-host on the console (the payload reads it at start).
"""
import argparse
import datetime
import socket


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8781)
    ap.add_argument("--out", default="netlog.txt")
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", args.port))
    print(f"listening on UDP {args.port}, saving to {args.out} (Ctrl+C to stop)", flush=True)
    with open(args.out, "a", encoding="utf-8") as out:
        out.write(f"===== {datetime.datetime.now().isoformat(timespec='seconds')} =====\n")
        try:
            while True:
                data, (ip, _) = s.recvfrom(4096)
                text = data.decode("utf-8", "replace").rstrip("\n")
                for line in text.split("\n"):
                    print(f"{ip} {line}", flush=True)
                    out.write(f"{ip} {line}\n")
                out.flush()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
