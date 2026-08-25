#!/usr/bin/env python3

import argparse
import socket
import struct
import time


ETHERTYPE = 0x88B5
ETHERTYPE_MSB = ETHERTYPE >> 8
ETHERTYPE_LSB = ETHERTYPE & 0xFF
MARKER = b"STM32H7 TX BENCH"
ETHERNET_HEADER_SIZE = 14
FCS_SIZE = 4
PREAMBLE_SFD_SIZE = 8
IFG_SIZE = 12


def format_mac(mac: bytes) -> str:
    return ":".join(f"{value:02x}" for value in mac)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Receive and verify STM32H7 raw Ethernet TX benchmark frames."
    )
    parser.add_argument("interface", help="wired interface, e.g. enp8s0")
    parser.add_argument("-n", "--count", type=int, default=200000)
    parser.add_argument("--size", type=int, default=60)
    parser.add_argument(
        "--start-timeout",
        type=float,
        default=15.0,
        help="seconds to wait for the first matching frame",
    )
    parser.add_argument(
        "--idle-timeout",
        type=float,
        default=2.0,
        help="seconds without a matching frame before stopping",
    )
    parser.add_argument(
        "--rcvbuf",
        type=int,
        default=16 * 1024 * 1024,
        help="requested socket receive buffer size in bytes",
    )
    args = parser.parse_args()

    if args.count <= 0:
        raise ValueError("count must be > 0")
    if args.size < 60 or args.size > 1514:
        raise ValueError("size must be 60..1514")
    if args.size < ETHERNET_HEADER_SIZE + len(MARKER) + 4:
        raise ValueError("size is too small for benchmark marker and sequence")

    sock = socket.socket(
        socket.AF_PACKET,
        socket.SOCK_RAW,
        socket.htons(ETHERTYPE),
    )
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.rcvbuf)
    sock.bind((args.interface, 0))
    sock.settimeout(0.2)

    actual_rcvbuf = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)

    print(f"Interface     : {args.interface}")
    print(f"Frame size    : {args.size}")
    print(f"Expected      : {args.count}")
    print(f"Socket rcvbuf : {actual_rcvbuf}")
    print("Waiting for STM32H7 TX benchmark frames...")

    seen = bytearray(args.count)
    recv_buffer = bytearray(2048)

    unique = 0
    duplicates = 0
    out_of_order = 0
    unexpected_sequence = 0
    wrong_size = 0

    first_time = None
    last_time = None
    first_source = None
    highest_sequence = -1

    wait_start = time.perf_counter()

    try:
        while unique < args.count:
            try:
                frame_length = sock.recv_into(recv_buffer)
            except socket.timeout:
                now = time.perf_counter()

                if first_time is None:
                    if now - wait_start >= args.start_timeout:
                        break
                elif last_time is not None and now - last_time >= args.idle_timeout:
                    break

                continue

            if frame_length < ETHERNET_HEADER_SIZE + len(MARKER) + 4:
                continue

            if (
                recv_buffer[12] != ETHERTYPE_MSB
                or recv_buffer[13] != ETHERTYPE_LSB
            ):
                continue

            if not recv_buffer.startswith(MARKER, ETHERNET_HEADER_SIZE):
                continue

            timestamp = time.perf_counter()

            if frame_length != args.size:
                wrong_size += 1
                continue

            sequence = struct.unpack_from(
                "!I", recv_buffer, frame_length - 4
            )[0]

            if first_time is None:
                first_time = timestamp
                first_source = bytes(recv_buffer[6:12])

            last_time = timestamp

            if sequence >= args.count:
                unexpected_sequence += 1
                continue

            if seen[sequence]:
                duplicates += 1
                continue

            seen[sequence] = 1
            unique += 1

            if sequence < highest_sequence:
                out_of_order += 1
            elif sequence > highest_sequence:
                highest_sequence = sequence
    finally:
        sock.close()

    missing = args.count - unique

    print()
    if first_source is not None:
        print(f"Source MAC    : {format_mac(first_source)}")
    print(f"Received      : {unique}")
    print(f"Missing       : {missing}")
    print(f"Duplicate     : {duplicates}")
    print(f"Out-of-order  : {out_of_order}")
    print(f"Unexpected seq: {unexpected_sequence}")
    print(f"Wrong size    : {wrong_size}")

    if first_time is not None and last_time is not None:
        elapsed = max(last_time - first_time, 1e-9)
        pps = unique / elapsed
        frame_rate_mbps = pps * args.size * 8 / 1_000_000
        on_wire_bytes = args.size + FCS_SIZE + PREAMBLE_SFD_SIZE + IFG_SIZE
        on_wire_mbps = pps * on_wire_bytes * 8 / 1_000_000

        print(f"Elapsed       : {elapsed:.6f} s")
        print(f"Actual PPS    : {pps:.1f}")
        print(f"Frame rate    : {frame_rate_mbps:.2f} Mbit/s")
        print(f"On-wire est.  : {on_wire_mbps:.2f} Mbit/s")

    if (
        missing == 0
        and duplicates == 0
        and out_of_order == 0
        and unexpected_sequence == 0
        and wrong_size == 0
    ):
        print("Result        : PASS")
    else:
        print("Result        : INCOMPLETE")


if __name__ == "__main__":
    main()
