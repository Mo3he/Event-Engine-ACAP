#!/usr/bin/env python3
"""Replay simple NMEA route samples over UDP or TCP for GPS input testing."""

import argparse
import socket
import sys
import time

DEFAULT_LINES = [
    "$GPRMC,163503.00,A,6325.279162,N,01024.042748,E,12.5,23.6,020726,0.3,W,A,V",
    "$GPGGA,163503.00,6325.279162,N,01024.042748,E,1,08,0.6,45.0,M,41.0,M,,",
    "$GPVTG,23.6,T,,M,12.5,N,23.2,K",
]


def frame(sentence: str, checksum: bool, eol: str) -> bytes:
    sentence = sentence.strip("\r\n")
    if checksum:
        body = sentence[1:] if sentence.startswith(("$", "!")) else sentence
        value = 0
        for char in body.split("*", 1)[0]:
            value ^= ord(char)
        sentence = f"{body.split('*', 1)[0]}*{value:02X}"
        sentence = "$" + sentence
    return (sentence + eol).encode("ascii")


def load_lines(path: str | None) -> list[str]:
    if not path:
        return DEFAULT_LINES
    with open(path, encoding="ascii") as source:
        lines = [line.strip() for line in source if line.strip()]
    if not lines:
        raise ValueError(f"{path}: no NMEA sentences found")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transport", choices=("udp", "tcp"), default="udp")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--rate", type=float, default=1.0,
                        help="complete sentence cycles per second (default: 1)")
    parser.add_argument("--file", help="optional file containing one sentence per line")
    parser.add_argument("--talker", default="", help="replace talker ID (e.g. GN)")
    parser.add_argument("--no-checksum", action="store_true")
    parser.add_argument("--lf-only", action="store_true")
    parser.add_argument("--once", action="store_true", help="send one cycle and exit")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.rate <= 0:
        parser.error("port must be 1..65535 and rate must be positive")

    try:
        lines = load_lines(args.file)
        eol = "\n" if args.lf_only else "\r\n"
        payloads = []
        for line in lines:
            if args.talker:
                if len(args.talker) != 2 or not args.talker.isalpha():
                    parser.error("--talker must be a two-letter ID, such as GN")
                if len(line) >= 6 and line[0] in "$!":
                    line = line[0] + args.talker.upper() + line[3:]
            payloads.append(frame(line, not args.no_checksum, eol))

        sock_type = socket.SOCK_DGRAM if args.transport == "udp" else socket.SOCK_STREAM
        with socket.socket(socket.AF_INET, sock_type) as sock:
            if args.transport == "tcp":
                sock.connect((args.host, args.port))
            print(f"Sending {len(payloads)} sentences/cycle to "
                  f"{args.host}:{args.port} via {args.transport} at {args.rate:g} Hz",
                  flush=True)
            deadline = time.monotonic()
            while True:
                for payload in payloads:
                    if args.transport == "udp":
                        sock.sendto(payload, (args.host, args.port))
                    else:
                        sock.sendall(payload)
                if args.once:
                    return 0
                deadline += 1.0 / args.rate
                time.sleep(max(0.0, deadline - time.monotonic()))
    except (OSError, UnicodeError, ValueError) as error:
        print(f"nmea_sim: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
