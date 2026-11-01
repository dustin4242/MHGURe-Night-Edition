#!/usr/bin/env python3
"""Probe an MHGURecomp Local Play host from another network.

This speaks the same two probes the game uses:
  - TCP: the same hello/Info exchange as Options -> Multiplayer -> Connect
  - UDP: the same discovery datagram as the game's LAN search
so it tells a firewall/port-forward problem apart from "the host is not hosting".

Usage:
    python3 ldn_probe.py mhgu.example.com:42500
    python3 ldn_probe.py 203.0.113.10:42500 --timeout 3

Run it from a VPS, a phone hotspot, or another network -- not from the host's own
LAN (router hairpin NAT can make that pass when the outside still cannot).
"""
import argparse
import socket
import struct
import sys

MAGIC = b"MHGL"
VERSION = 2
NETINFO_SIZE = 0x480
F_HELLO, F_INFO, F_WELCOME, F_REFUSE = 1, 2, 3, 4
NI_SSID, NI_NODE_MAX, NI_NODE_COUNT = 0x26, 0x66, 0x67


def split_host(value):
    value = value.strip()
    if value.startswith("["):  # [v6]:port -- the runtime only resolves IPv4, but be clear
        end = value.find("]")
        host = value[1:end]
        rest = value[end + 1:]
        port = int(rest[1:]) if rest.startswith(":") else 39500
        return host, port
    if ":" in value:
        host, port = value.rsplit(":", 1)
        return host, int(port)
    return value, 39500


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        part = sock.recv(n - len(buf))
        if not part:
            raise ConnectionError("connection closed")
        buf += part
    return buf


def parse_info(info):
    ssid = info[NI_SSID:NI_SSID + 0x22].split(b"\0", 1)[0].decode("utf-8", "replace")
    node_max = info[NI_NODE_MAX]
    node_count = info[NI_NODE_COUNT] if len(info) > NI_NODE_COUNT else 0
    return ssid, node_count, node_max


def resolve(host, port, family, kind):
    try:
        return socket.getaddrinfo(host, port, family, kind, 0, socket.AI_ADDRCONFIG)
    except socket.gaierror as e:
        print(f"DNS:     FAILED for {host}: {e}")
        print("         Add an IPv4 A record (the game resolves IPv4 only); check the DDNS value.")
        return []


def tcp_probe(host, port, timeout):
    print(f"Target:  {host}:{port}")
    addrs = resolve(host, port, socket.AF_INET, socket.SOCK_STREAM)
    if not addrs:
        return 2
    ip, port = addrs[0][4][0], addrs[0][4][1]
    print(f"Resolved IPv4: {ip}:{port}")

    # Hello { magic, version, scan_only=1, pad, lcv, mac[6], name[0x21], pad[3] }
    hello = struct.pack("<4sHBBH6s33s3s", MAGIC, VERSION, 1, 0, 0, b"\0" * 6, b"\0" * 33, b"\0" * 3)
    frame = struct.pack("<I", 1 + len(hello)) + b"\x01" + hello
    try:
        s = socket.create_connection((ip, port), timeout)
    except OSError as e:
        print(f"TCP:     FAILED to connect ({e})")
        print("         Host not in a Hub, Linux/Windows firewall, router forwarding, or CGNAT.")
        return 3
    print("TCP:     connected")
    try:
        s.settimeout(timeout)
        s.sendall(frame)
        size = struct.unpack("<I", recv_exact(s, 4))[0]
        if size < 1 or size > 0x10041:
            print(f"TCP:     bad reply size {size}")
            return 4
        rtype = recv_exact(s, 1)[0]
        payload = recv_exact(s, size - 1)
    except (OSError, ConnectionError) as e:
        print(f"TCP:     connected but no reply ({e})")
        print("         The host is not hosting a Hub, or this is another service/port.")
        return 4
    finally:
        s.close()

    if rtype == F_REFUSE:
        result = struct.unpack_from("<I", payload)[0] if len(payload) >= 4 else 0
        print(f"TCP:     REFUSED (result {result:#x}) -- likely a different build/protocol version")
        return 5
    if rtype != F_INFO:
        print(f"TCP:     unexpected reply type {rtype} ({len(payload)} bytes)")
        return 4
    if len(payload) < NETINFO_SIZE:
        print(f"TCP:     F_INFO is short ({len(payload)} < {NETINFO_SIZE} bytes)")
        return 4
    ssid, count, maximum = parse_info(payload)
    print(f"TCP:     OK -- local-play host answered")
    print(f"Hub:     {ssid!r}  players {count}/{maximum}")
    return 0


def udp_probe(host, port, timeout):
    addrs = resolve(host, port, socket.AF_INET, socket.SOCK_DGRAM)
    if not addrs:
        return
    ip, port = addrs[0][4][0], addrs[0][4][1]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    probe = MAGIC + struct.pack("<HH", VERSION, 0)
    try:
        for _ in range(3):
            s.sendto(probe, (ip, port))
            try:
                data, _addr = s.recvfrom(0x10000)
            except socket.timeout:
                continue
            if len(data) >= 8 + NETINFO_SIZE and data[:4] == MAGIC and struct.unpack_from("<H", data, 4)[0] == VERSION:
                ssid, count, maximum = parse_info(data[8:8 + NETINFO_SIZE])
                print(f"UDP:     OK -- discovery reply from {ip}:{port} ({ssid!r}, {count}/{maximum} players)")
                return
            print(f"UDP:     reply from {ip}:{port} but not a local-play discovery packet")
            return
        print("UDP:     no discovery reply")
        print("         Forward UDP as well as TCP; the game can fall back to TCP after joining,")
        print("         but the LAN-search path uses UDP.")
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description="Probe an MHGURecomp Local Play host")
    ap.add_argument("host", help="host or domain, optionally with :port (default 39500)")
    ap.add_argument("--timeout", type=float, default=3.0, help="seconds per step (default 3)")
    args = ap.parse_args()
    host, port = split_host(args.host)
    rc = tcp_probe(host, port, args.timeout)
    udp_probe(host, port, args.timeout)
    return rc


if __name__ == "__main__":
    sys.exit(main())
