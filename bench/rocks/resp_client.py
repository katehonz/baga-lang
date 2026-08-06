#!/usr/bin/env python3
"""Minimal RESP2 client microbench (no redis-py required).

Modes:
  --pipe 1      one command / RTT (default)
  --pipe N      pipeline N commands per flush (Redis-style bulk)
  --clients C   C concurrent connections (R36 multi-conn soak)

Machine-readable lines:
  META host=… port=… n=… vlen=… pipe=… clients=…
  PING_MS=… PING_OPS=…
  SET_MS=… SET_OPS=…
  GET_MS=… GET_OPS=…
  OK

With clients>1: n is per-client op count; OPS is aggregate wall-clock throughput.
Keys are namespaced c{cid}:k{i} so writers do not thrash the same key.
"""
from __future__ import annotations

import argparse
import os
import socket
import threading
import time
from typing import Callable


def enc_cmd(*parts: str) -> bytes:
    out = [f"*{len(parts)}\r\n".encode()]
    for p in parts:
        b = p.encode() if isinstance(p, str) else p
        out.append(f"${len(b)}\r\n".encode())
        out.append(b)
        out.append(b"\r\n")
    return b"".join(out)


class Resp:
    def __init__(self, host: str, port: int, timeout: float = 120.0):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = bytearray()

    def close(self) -> None:
        try:
            self.s.close()
        except OSError:
            pass

    def _read_more(self) -> None:
        chunk = self.s.recv(65536)
        if not chunk:
            raise EOFError("connection closed")
        self.buf.extend(chunk)

    def _readline(self) -> bytes:
        while True:
            i = self.buf.find(b"\r\n")
            if i >= 0:
                line = bytes(self.buf[:i])
                del self.buf[: i + 2]
                return line
            self._read_more()

    def _read_exact(self, n: int) -> bytes:
        while len(self.buf) < n:
            self._read_more()
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def read_reply(self):
        line = self._readline()
        if not line:
            raise ValueError("empty reply")
        t = line[:1]
        if t == b"+":
            return line[1:].decode()
        if t == b"-":
            raise RuntimeError(line[1:].decode())
        if t == b":":
            return int(line[1:])
        if t == b"$":
            n = int(line[1:])
            if n < 0:
                return None
            data = self._read_exact(n)
            crlf = self._read_exact(2)
            if crlf != b"\r\n":
                raise ValueError("bulk framing")
            return data
        if t == b"*":
            n = int(line[1:])
            if n < 0:
                return None
            return [self.read_reply() for _ in range(n)]
        raise ValueError(f"bad type {line!r}")

    def call(self, *parts: str):
        self.s.sendall(enc_cmd(*parts))
        return self.read_reply()

    def pipeline(self, commands: list[tuple]) -> list:
        """Send all commands, then read all replies (true pipeline)."""
        if not commands:
            return []
        blob = b"".join(enc_cmd(*c) for c in commands)
        self.s.sendall(blob)
        return [self.read_reply() for _ in commands]


def ops_per_s(n: int, sec: float) -> int:
    if sec <= 0:
        return n * 1_000_000
    return int(n / sec)


def chunked(n: int, pipe: int):
    i = 0
    while i < n:
        yield i, min(pipe, n - i)
        i += pipe


def key_name(cid: int, i: int, clients: int) -> str:
    if clients <= 1:
        return f"k{i:08d}"
    return f"c{cid}:k{i:08d}"


def run_phase_ping(r: Resp, n: int, pipe: int) -> None:
    if pipe == 1:
        for _ in range(n):
            if r.call("PING") != "PONG":
                raise RuntimeError("bad PING")
    else:
        for _, batch in chunked(n, pipe):
            cmds = [("PING",)] * batch
            for rep in r.pipeline(cmds):
                if rep != "PONG":
                    raise RuntimeError(f"bad PING {rep!r}")


def run_phase_set(r: Resp, n: int, pipe: int, val: str, cid: int, clients: int) -> None:
    if pipe == 1:
        for i in range(n):
            if r.call("SET", key_name(cid, i, clients), val) != "OK":
                raise RuntimeError("bad SET")
    else:
        for start, batch in chunked(n, pipe):
            cmds = [
                ("SET", key_name(cid, start + j, clients), val) for j in range(batch)
            ]
            for rep in r.pipeline(cmds):
                if rep != "OK":
                    raise RuntimeError(f"bad SET {rep!r}")


def run_phase_get(r: Resp, n: int, pipe: int, val: str, cid: int, clients: int) -> None:
    if pipe == 1:
        for i in range(n):
            v = r.call("GET", key_name(cid, i, clients))
            if v is None or len(v) != len(val):
                raise RuntimeError(f"bad GET {i}")
    else:
        for start, batch in chunked(n, pipe):
            cmds = [
                ("GET", key_name(cid, start + j, clients)) for j in range(batch)
            ]
            for j, rep in enumerate(r.pipeline(cmds)):
                if rep is None or len(rep) != len(val):
                    raise RuntimeError(f"bad GET {start + j}")


def wall_multi(
    host: str,
    port: int,
    clients: int,
    worker: Callable[[int, Resp], None],
) -> float:
    """Run worker(cid, conn) on each client; return wall seconds."""
    errors: list[BaseException] = []
    barrier = threading.Barrier(clients)

    def thr(cid: int) -> None:
        r = None
        try:
            r = Resp(host, port)
            barrier.wait(timeout=30)
            worker(cid, r)
        except BaseException as e:
            errors.append(e)
        finally:
            if r is not None:
                r.close()

    threads = [threading.Thread(target=thr, args=(c,), daemon=True) for c in range(clients)]
    t0 = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    t1 = time.perf_counter()
    if errors:
        raise errors[0]
    return t1 - t0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.environ.get("BENCH_HOST", "127.0.0.1"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("BENCH_PORT", "16579")))
    ap.add_argument("--n", type=int, default=int(os.environ.get("BENCH_N", "5000")))
    ap.add_argument("--vlen", type=int, default=int(os.environ.get("BENCH_VLEN", "100")))
    ap.add_argument(
        "--pipe",
        type=int,
        default=int(os.environ.get("BENCH_PIPE", "1")),
        help="pipeline width (1 = one RTT per command)",
    )
    ap.add_argument(
        "--clients",
        type=int,
        default=int(os.environ.get("BENCH_CLIENTS", "1")),
        help="concurrent connections (R36 multi-conn soak)",
    )
    args = ap.parse_args()

    n = max(args.n, 100)
    pipe = max(args.pipe, 1)
    clients = max(args.clients, 1)
    val = "x" * max(args.vlen, 1)
    total = n * clients

    print(
        f"META host={args.host} port={args.port} n={n} vlen={len(val)} "
        f"pipe={pipe} clients={clients}"
    )

    if clients == 1:
        r = Resp(args.host, args.port)
        t0 = time.perf_counter()
        run_phase_ping(r, n, pipe)
        ping_s = time.perf_counter() - t0
        print(f"PING_MS={int(ping_s * 1000)} PING_OPS={ops_per_s(n, ping_s)}")

        t0 = time.perf_counter()
        run_phase_set(r, n, pipe, val, 0, 1)
        set_s = time.perf_counter() - t0
        print(f"SET_MS={int(set_s * 1000)} SET_OPS={ops_per_s(n, set_s)}")

        t0 = time.perf_counter()
        run_phase_get(r, n, pipe, val, 0, 1)
        get_s = time.perf_counter() - t0
        print(f"GET_MS={int(get_s * 1000)} GET_OPS={ops_per_s(n, get_s)}")
        r.close()
    else:
        # Aggregate wall-clock throughput across concurrent clients.
        ping_s = wall_multi(
            args.host,
            args.port,
            clients,
            lambda cid, r: run_phase_ping(r, n, pipe),
        )
        print(f"PING_MS={int(ping_s * 1000)} PING_OPS={ops_per_s(total, ping_s)}")

        set_s = wall_multi(
            args.host,
            args.port,
            clients,
            lambda cid, r: run_phase_set(r, n, pipe, val, cid, clients),
        )
        print(f"SET_MS={int(set_s * 1000)} SET_OPS={ops_per_s(total, set_s)}")

        get_s = wall_multi(
            args.host,
            args.port,
            clients,
            lambda cid, r: run_phase_get(r, n, pipe, val, cid, clients),
        )
        print(f"GET_MS={int(get_s * 1000)} GET_OPS={ops_per_s(total, get_s)}")

    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
