#!/usr/bin/env python3

import argparse
import socket
import struct
import time


ETHERTYPE = 0x88B5
FRAME_SIZE = 60          # 不包含 FCS，满足 Ethernet 最小帧长度
DEFAULT_COUNT = 1000
DEFAULT_INTERVAL_MS = 5.0


def get_interface_mac(interface: str) -> bytes:
    """从 AF_PACKET socket 获取网卡 MAC 地址。"""
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    try:
        sock.bind((interface, 0))
        info = sock.getsockname()
        return info[4]
    finally:
        sock.close()


def build_frame(src_mac: bytes, sequence: int) -> bytes:
    """构造一个广播 0x88B5 Ethernet Frame。"""
    dst_mac = b"\xff\xff\xff\xff\xff\xff"

    ethernet_header = (
        dst_mac
        + src_mac
        + struct.pack("!H", ETHERTYPE)
    )

    payload = bytearray(FRAME_SIZE - len(ethernet_header))

    # 固定标识，抓包时比较容易认
    marker = b"STM32H7 RX TEST"
    payload[:len(marker)] = marker

    # 最后 4 字节保存序号
    payload[-4:] = struct.pack("!I", sequence)

    return ethernet_header + payload


def format_mac(mac: bytes) -> str:
    return ":".join(f"{byte:02x}" for byte in mac)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="向 STM32H7 发送 Ethernet Raw RX 测试帧"
    )
    parser.add_argument(
        "interface",
        help="发送网卡，例如 enp3s0",
    )
    parser.add_argument(
        "-n",
        "--count",
        type=int,
        default=DEFAULT_COUNT,
        help=f"发送帧数，默认 {DEFAULT_COUNT}",
    )
    parser.add_argument(
        "-i",
        "--interval-ms",
        type=float,
        default=DEFAULT_INTERVAL_MS,
        help=f"帧间隔 ms，默认 {DEFAULT_INTERVAL_MS}",
    )

    args = parser.parse_args()

    src_mac = get_interface_mac(args.interface)

    sock = socket.socket(
        socket.AF_PACKET,
        socket.SOCK_RAW,
        socket.htons(ETHERTYPE),
    )
    sock.bind((args.interface, 0))

    print(f"Interface : {args.interface}")
    print(f"Source MAC: {format_mac(src_mac)}")
    print("Dest MAC  : ff:ff:ff:ff:ff:ff")
    print(f"EtherType : 0x{ETHERTYPE:04X}")
    print(f"Count     : {args.count}")
    print(f"Interval  : {args.interval_ms} ms")
    print()

    interval_s = args.interval_ms / 1000.0

    try:
        for sequence in range(args.count):
            frame = build_frame(src_mac, sequence)
            sock.send(frame)

            sent = sequence + 1

            if sent % 100 == 0 or sent == args.count:
                print(f"sent {sent}/{args.count}")

            if interval_s > 0:
                time.sleep(interval_s)

    finally:
        sock.close()

    print("Done.")


if __name__ == "__main__":
    main()
