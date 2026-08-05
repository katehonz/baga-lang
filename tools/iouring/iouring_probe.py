#!/usr/bin/env python3
"""iouring_probe.py — Phase 5 io_uring sketch (no liburing).

x86-64 Linux only. Exercises:
  1) SYS_io_uring_setup feature detect
  2) map rings + submit IORING_OP_NOP
  3) optional IORING_OP_POLL_ADD on a self-pipe (write end ready → POLLIN)

Exit 0 on success; 2 if kernel/arch unsupported; 1 on unexpected failure.
"""
from __future__ import annotations

import argparse
import ctypes
import mmap
import os
import struct
import sys

# --- x86-64 Linux numbers (from linux/io_uring.h / unistd_64.h) ---
SYS_io_uring_setup = 425
SYS_io_uring_enter = 426
SYS_io_uring_register = 427  # unused in sketch

IORING_OFF_SQ_RING = 0
IORING_OFF_CQ_RING = 0x8000000
IORING_OFF_SQES = 0x10000000

IORING_OP_NOP = 0
IORING_OP_POLL_ADD = 6

IORING_ENTER_GETEVENTS = 1 << 0

POLLIN = 0x0001

# sizeof(struct io_uring_sqe) == 64 on common kernels
SQE_SIZE = 64
# sizeof(struct io_uring_cqe) == 16
CQE_SIZE = 16

# io_uring_params is 120 bytes on typical x86-64 (sq_off/cq_off 40 each + head)
PARAMS_SIZE = 120


def _libc():
    lib = ctypes.CDLL(None, use_errno=True)
    lib.syscall.restype = ctypes.c_long
    return lib


def _errno() -> int:
    return ctypes.get_errno()


def io_uring_setup(entries: int) -> tuple[int, bytes]:
    """Return (ring_fd, params_bytes) or raise OSError."""
    lib = _libc()
    params = (ctypes.c_uint8 * PARAMS_SIZE)()
    ctypes.memset(params, 0, PARAMS_SIZE)
    # entries in a1; params pointer in a2
    fd = lib.syscall(SYS_io_uring_setup, entries, ctypes.byref(params))
    if fd < 0:
        raise OSError(_errno(), f"io_uring_setup failed errno={_errno()}")
    return int(fd), bytes(params)


def _u32(b: bytes, off: int) -> int:
    return struct.unpack_from("<I", b, off)[0]


def parse_params(p: bytes) -> dict:
    """Minimal parse of io_uring_params + sq/cq offsets."""
    # layout: sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle,
    # features, wq_fd, resv[3]  → 40 bytes, then sq_off (40), cq_off (40)
    sq_entries = _u32(p, 0)
    cq_entries = _u32(p, 4)
    flags = _u32(p, 8)
    features = _u32(p, 20)
    # struct io_sqring_offsets at 40:
    # head, tail, ring_mask, ring_entries, flags, dropped, array, resv1, user_addr
    base = 40
    sq = {
        "head": _u32(p, base + 0),
        "tail": _u32(p, base + 4),
        "ring_mask": _u32(p, base + 8),
        "ring_entries": _u32(p, base + 12),
        "flags": _u32(p, base + 16),
        "dropped": _u32(p, base + 20),
        "array": _u32(p, base + 24),
    }
    base = 80
    cq = {
        "head": _u32(p, base + 0),
        "tail": _u32(p, base + 4),
        "ring_mask": _u32(p, base + 8),
        "ring_entries": _u32(p, base + 12),
        "overflow": _u32(p, base + 16),
        "cqes": _u32(p, base + 20),
        "flags": _u32(p, base + 24),
    }
    return {
        "sq_entries": sq_entries,
        "cq_entries": cq_entries,
        "flags": flags,
        "features": features,
        "sq": sq,
        "cq": cq,
    }


def io_uring_enter(fd: int, to_submit: int, min_complete: int, flags: int) -> int:
    lib = _libc()
    # long syscall(SYS_io_uring_enter, fd, to_submit, min_complete, flags, sig, sigsz)
    # sig=NULL, sigsz=0
    rc = lib.syscall(
        SYS_io_uring_enter,
        fd,
        to_submit,
        min_complete,
        flags,
        0,
        0,
    )
    if rc < 0:
        raise OSError(_errno(), f"io_uring_enter failed errno={_errno()}")
    return int(rc)


class Ring:
    def __init__(self, entries: int = 8):
        self.fd, raw = io_uring_setup(entries)
        self.params = parse_params(raw)
        sqe = self.params["sq"]
        cqe = self.params["cq"]
        self.sq_entries = self.params["sq_entries"]
        self.cq_entries = self.params["cq_entries"]

        # Map sizes: kernel docs use offsets; size ≈ array end or cqes end
        sq_ring_sz = sqe["array"] + self.sq_entries * 4
        cq_ring_sz = cqe["cqes"] + self.cq_entries * CQE_SIZE
        # Single map covering SQ ring region at IORING_OFF_SQ_RING
        self.sq_map = mmap.mmap(
            self.fd,
            max(sq_ring_sz, 4096),
            mmap.MAP_SHARED,
            mmap.PROT_READ | mmap.PROT_WRITE,
            offset=IORING_OFF_SQ_RING,
        )
        self.cq_map = mmap.mmap(
            self.fd,
            max(cq_ring_sz, 4096),
            mmap.MAP_SHARED,
            mmap.PROT_READ | mmap.PROT_WRITE,
            offset=IORING_OFF_CQ_RING,
        )
        self.sqes = mmap.mmap(
            self.fd,
            max(self.sq_entries * SQE_SIZE, 4096),
            mmap.MAP_SHARED,
            mmap.PROT_READ | mmap.PROT_WRITE,
            offset=IORING_OFF_SQES,
        )
        self._sq = sqe
        self._cq = cqe

    def close(self) -> None:
        for m in (self.sq_map, self.cq_map, self.sqes):
            try:
                m.close()
            except Exception:
                pass
        os.close(self.fd)

    def _sq_head(self) -> int:
        return struct.unpack_from("<I", self.sq_map, self._sq["head"])[0]

    def _sq_tail(self) -> int:
        return struct.unpack_from("<I", self.sq_map, self._sq["tail"])[0]

    def _set_sq_tail(self, v: int) -> None:
        struct.pack_into("<I", self.sq_map, self._sq["tail"], v)

    def _cq_head(self) -> int:
        return struct.unpack_from("<I", self.cq_map, self._cq["head"])[0]

    def _cq_tail(self) -> int:
        return struct.unpack_from("<I", self.cq_map, self._cq["tail"])[0]

    def _set_cq_head(self, v: int) -> None:
        struct.pack_into("<I", self.cq_map, self._cq["head"], v)

    def _sq_mask(self) -> int:
        return struct.unpack_from("<I", self.sq_map, self._sq["ring_mask"])[0]

    def _cq_mask(self) -> int:
        return struct.unpack_from("<I", self.cq_map, self._cq["ring_mask"])[0]

    def submit_sqe(self, opcode: int, fd: int, user_data: int, poll_events: int = 0) -> None:
        """Queue one SQE at current tail (must have room)."""
        tail = self._sq_tail()
        head = self._sq_head()
        mask = self._sq_mask()
        if (tail - head) >= self.sq_entries:
            raise RuntimeError("SQ full")
        idx = tail & mask
        off = idx * SQE_SIZE
        # zero SQE
        self.sqes[off : off + SQE_SIZE] = b"\x00" * SQE_SIZE
        # opcode u8, flags u8, ioprio u16, fd i32
        struct.pack_into("<BBHI", self.sqes, off, opcode, 0, 0, fd & 0xFFFFFFFF)
        # off/addr2 u64 @8, addr u64 @16, len u32 @24, union flags @28
        # for POLL_ADD: poll_events in the union at offset 28 (as u16/u32)
        if opcode == IORING_OP_POLL_ADD:
            struct.pack_into("<I", self.sqes, off + 28, poll_events & 0xFFFFFFFF)
        # user_data u64 @32
        struct.pack_into("<Q", self.sqes, off + 32, user_data & 0xFFFFFFFFFFFFFFFF)
        # array slot
        array_off = self._sq["array"] + idx * 4
        struct.pack_into("<I", self.sq_map, array_off, idx)
        self._set_sq_tail(tail + 1)

    def enter_and_get(self, nsubmit: int, min_complete: int) -> list[tuple[int, int]]:
        io_uring_enter(self.fd, nsubmit, min_complete, IORING_ENTER_GETEVENTS)
        out: list[tuple[int, int]] = []
        head = self._cq_head()
        tail = self._cq_tail()
        mask = self._cq_mask()
        cqes_off = self._cq["cqes"]
        while head != tail:
            i = head & mask
            base = cqes_off + i * CQE_SIZE
            user_data = struct.unpack_from("<Q", self.cq_map, base)[0]
            res = struct.unpack_from("<i", self.cq_map, base + 8)[0]
            out.append((user_data, res))
            head += 1
        self._set_cq_head(head)
        return out


def cmd_detect() -> int:
    try:
        fd, raw = io_uring_setup(2)
    except OSError as e:
        print(f"io_uring: unsupported or denied ({e})")
        return 2
    p = parse_params(raw)
    os.close(fd)
    print(
        f"io_uring: ok setup fd was live; "
        f"sq_entries={p['sq_entries']} cq_entries={p['cq_entries']} "
        f"features=0x{p['features']:x}"
    )
    return 0


def cmd_nop() -> int:
    r = Ring(8)
    try:
        r.submit_sqe(IORING_OP_NOP, -1, user_data=0xBA6A0001)
        cqes = r.enter_and_get(1, 1)
        if not cqes:
            print("FAIL: no CQE for NOP")
            return 1
        ud, res = cqes[0]
        if ud != 0xBA6A0001 or res != 0:
            print(f"FAIL: NOP cqe user_data={ud:#x} res={res}")
            return 1
        print("io_uring NOP: ok")
        return 0
    finally:
        r.close()


def cmd_poll_pipe() -> int:
    """POLL_ADD on read end of pipe after writing one byte."""
    r = Ring(8)
    rfd, wfd = os.pipe()
    try:
        # arm poll on read end
        r.submit_sqe(IORING_OP_POLL_ADD, rfd, user_data=rfd, poll_events=POLLIN)
        # make readable
        os.write(wfd, b"x")
        cqes = r.enter_and_get(1, 1)
        if not cqes:
            print("FAIL: no CQE for POLL_ADD")
            return 1
        ud, res = cqes[0]
        if ud != rfd:
            print(f"FAIL: user_data={ud} expected fd={rfd}")
            return 1
        if res < 0:
            print(f"FAIL: POLL_ADD res={res}")
            return 1
        # res is poll mask (often POLLIN)
        if (res & POLLIN) == 0 and res != 0:
            # some kernels return the mask; 0 with success is unexpected
            print(f"WARN: res={res:#x} (no POLLIN bit); accepting non-neg")
        print(f"io_uring POLL_ADD pipe: ok res={res:#x} fd={rfd}")
        return 0
    finally:
        r.close()
        os.close(rfd)
        os.close(wfd)


def main(argv: list[str] | None = None) -> int:
    if sys.platform != "linux" or os.uname().machine not in ("x86_64", "amd64"):
        print("io_uring sketch: only x86_64 Linux supported")
        return 2
    ap = argparse.ArgumentParser(description="Baga Phase 5 io_uring probe (no liburing)")
    ap.add_argument(
        "cmd",
        nargs="?",
        default="all",
        choices=("detect", "nop", "poll", "all"),
        help="detect | nop | poll | all (default)",
    )
    args = ap.parse_args(argv)
    steps = {
        "detect": cmd_detect,
        "nop": cmd_nop,
        "poll": cmd_poll_pipe,
    }
    if args.cmd == "all":
        for name in ("detect", "nop", "poll"):
            rc = steps[name]()
            if rc != 0:
                return rc
        print("iouring_probe: all passed")
        return 0
    return steps[args.cmd]()


if __name__ == "__main__":
    sys.exit(main())
