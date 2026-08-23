#!/usr/bin/env python3

import argparse
import socket
import struct
import time


ETHERTYPE = 0x88B5
MIN_FRAME_SIZE = 60
MAX_FRAME_SIZE = 1514


def get_interface_mac(interface: str) -> bytes:
    """读取网卡 MAC 地址。"""
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)

    try:
        sock.bind((interface, 0))
        return sock.getsockname()[4]
    finally:
        sock.close()


def format_mac(mac: bytes) -> str:
    """将 MAC 地址转换为可读字符串。"""
    return ":".join(f"{value:02x}" for value in mac)


def build_frame(
    src_mac: bytes,
    frame_size: int,
    sequence: int,
) -> bytes:
    """构造测试 Ethernet Frame。"""
    dst_mac = b"\xff\xff\xff\xff\xff\xff"

    header = (
        dst_mac
        + src_mac
        + struct.pack("!H", ETHERTYPE)
    )

    payload = bytearray(frame_size - len(header))

    marker = b"STM32H7 RX STRESS"
    payload[:len(marker)] = marker

    payload[-4:] = struct.pack("!I", sequence)

    return header + payload


def wait_until(target_ns: int) -> None:
    """等待到指定单调时钟时间点。"""
    while True:
        now_ns = time.perf_counter_ns()
        remaining_ns = target_ns - now_ns

        if remaining_ns <= 0:
            return

        if remaining_ns > 500_000:
            time.sleep(
                (remaining_ns - 200_000) / 1_000_000_000
            )


def run_rate_test(
    sock: socket.socket,
    src_mac: bytes,
    frame_size: int,
    count: int,
    pps: float,
) -> None:
    """按指定 PPS 连续发送测试帧。"""
    period_ns = 0

    if pps > 0:
        period_ns = int(1_000_000_000 / pps)

    start_ns = time.perf_counter_ns()
    deadline_ns = start_ns

    for sequence in range(count):
        frame = build_frame(
            src_mac,
            frame_size,
            sequence,
        )

        sock.send(frame)

        if period_ns > 0:
            deadline_ns += period_ns
            wait_until(deadline_ns)

    end_ns = time.perf_counter_ns()

    elapsed_s = (end_ns - start_ns) / 1_000_000_000
    actual_pps = count / elapsed_s

    print()
    print(f"Sent       : {count}")
    print(f"Elapsed    : {elapsed_s:.3f} s")
    print(f"Actual PPS : {actual_pps:.1f}")
    print(
        f"Frame rate : "
        f"{actual_pps * frame_size * 8 / 1_000_000:.2f} Mbit/s"
    )


def run_burst_test(
    sock: socket.socket,
    src_mac: bytes,
    frame_size: int,
    count: int,
    burst_size: int,
    burst_gap_ms: float,
) -> None:
    """按 burst 模式发送测试帧。"""
    start_ns = time.perf_counter_ns()
    sequence = 0

    while sequence < count:
        current_burst = min(
            burst_size,
            count - sequence,
        )

        for _ in range(current_burst):
            frame = build_frame(
                src_mac,
                frame_size,
                sequence,
            )

            sock.send(frame)
            sequence += 1

        if sequence < count:
            time.sleep(burst_gap_ms / 1000.0)

    end_ns = time.perf_counter_ns()

    elapsed_s = (end_ns - start_ns) / 1_000_000_000

    print()
    print(f"Sent       : {count}")
    print(f"Burst size : {burst_size}")
    print(f"Burst gap  : {burst_gap_ms} ms")
    print(f"Elapsed    : {elapsed_s:.3f} s")
    print(f"Actual PPS : {count / elapsed_s:.1f}")


def main() -> None:
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "interface",
        help="有线网卡，例如 enp4s0",
    )

    parser.add_argument(
        "-n",
        "--count",
        type=int,
        default=10000,
    )

    parser.add_argument(
        "--size",
        type=int,
        default=60,
    )

    parser.add_argument(
        "--pps",
        type=float,
        default=1000.0,
        help="目标 PPS，0 表示尽可能快",
    )

    parser.add_argument(
        "--burst-size",
        type=int,
        default=0,
        help="非 0 时启用 burst 模式",
    )

    parser.add_argument(
        "--burst-gap-ms",
        type=float,
        default=1.0,
    )

    args = parser.parse_args()

    if not MIN_FRAME_SIZE <= args.size <= MAX_FRAME_SIZE:
        raise ValueError(
            f"frame size must be "
            f"{MIN_FRAME_SIZE}..{MAX_FRAME_SIZE}"
        )

    src_mac = get_interface_mac(args.interface)

    sock = socket.socket(
        socket.AF_PACKET,
        socket.SOCK_RAW,
        socket.htons(ETHERTYPE),
    )

    sock.bind((args.interface, 0))

    print(f"Interface  : {args.interface}")
    print(f"Source MAC : {format_mac(src_mac)}")
    print(f"Frame size : {args.size}")
    print(f"Count      : {args.count}")

    try:
        if args.burst_size > 0:
            run_burst_test(
                sock,
                src_mac,
                args.size,
                args.count,
                args.burst_size,
                args.burst_gap_ms,
            )
        else:
            run_rate_test(
                sock,
                src_mac,
                args.size,
                args.count,
                args.pps,
            )
    finally:
        sock.close()


if __name__ == "__main__":
    main()
