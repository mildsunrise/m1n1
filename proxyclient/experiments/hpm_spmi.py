#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import sys, pathlib
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from m1n1.setup import *
from m1n1.hw.spmi import SPMI

hpm_reg_sizes = (
    #0    1    2    3    4    5    6    7    8    9    a    b    c    d    e    f

     4,   4,   4,   4,   4,  16,   0,   0,   8,  64,   0,   0,   0,   0,   0,   8,
     8,  64,   4,   4,  13,  13,  11,  11,  11,  11,   4,  13,  11,  11,   8,  64,
     1,   6,   6,   4,   9,   0,   4,   8,  64,  64,   8,  36,  48,  52,   0,  64,
    61,  53,  51,  45,   0,  18,  12,  12,  10,   4,   4,  16,  36,  36,  64,   6,

    14,  49,  34,  26,  17,  21,  25,  49,  61,  61,  54,  64,  64,  29,  29,  29,
     6,   7,   7,   0,   0,   0,   7,  64,  37,  11,   1,   0,   0,   5,  22,   4,
    64,  64,  64,   4,  64,  64,  64,   4,  56,  16,  56,  64,  64,  64,  20,   0,
     0,  52,  64,  50,  55,   0,   0,   0,  40,  64,  56,  46,  12,  64,  33,  64,
)

class HpmSPMI:
    def __init__(self, spmi: SPMI, slave: int):
        self.spmi = spmi
        self.slave = slave
        self.wakeup()

        self.write(0x16, ( (~((~0) << 11)) & ~(1<<44) ).to_bytes(11, 'little'))
        self.write(0x17, ( (~((~0) << 11)) & ~(1<<44) ).to_bytes(11, 'little'))

        if events := self.read_events():
            print('events:', events)
        events = self.read_events()
        assert not events, events

    def wakeup(self):
        reply = self.spmi.wakeup(self.slave)
        assert reply

        for i in range(10):
            reply = self.spmi.write_zero(self.slave, 3)
            assert reply
            reg_read = self.spmi.read_reg(self.slave, 0)
            if reg_read == 3: return
        raise Exception('did not wake up')

    def select(self, reg: int):
        reply = self.spmi.write_zero(self.slave, reg)
        assert reply
        reg_read = self.spmi.read_reg(self.slave, 0)
        assert reg_read == reg, f'failed to confirm selection: expected {reg:#x}, got {reg_read:#x}'

    def get_size(self, reg: int):
        self.select(reg)
        size = self.spmi.read_reg(self.slave, 0x1F)
        assert size <= 64
        return size

    def read(self, reg: int, size=-1):
        exp_size = self.get_size(reg)
        if size == -1:
            size = exp_size
        assert size <= exp_size

        data = b''
        while len(data) < size:
            data += self.spmi.read_ext(self.slave, 0x20 + len(data), min(size - len(data), 16))
        return data

    def write(self, reg: int, data: bytes):
        size = self.get_size(reg)
        assert len(data) <= size, f'invalid size: passed {len(data)}, register is {size}'
        written = 0
        while written < len(data):
            to_write = min(len(data) - written, 16)
            reply = self.spmi.write_ext(self.slave, 0xA0 + written, data[written:written + to_write])
            assert reply
            written += to_write
        self.select(reg) # issue write

    def command(self, cmd: bytes, data: bytes, out_size: int):
        self.write(9, data)

        assert len(cmd) == 4
        self.write(8, cmd)

        self.select(8)
        while any(rep := self.spmi.read_ext(self.slave, 0x20, 4)):
            assert rep != b'!CMD', 'invalid command'
            assert rep == cmd

        return self.read(9, out_size)

    def read_events(self):
        evts = b''
        for base in (0x14, 0x15):
            e = self.read(base, 9)
            self.write(base + 4, e)
            evts += e

        evts = int.from_bytes(evts, 'little')
        bits = set()
        while evts:
            bits.add(bit := evts.bit_length() - 1)
            evts &= ~(1 << bit)
        return bits

def trim_zeros(x):
    i = len(x)
    while i and not x[i-1]: i -= 1
    return x[:i]

for node in u.adt._adt.walk_tree():
    if not (hasattr(node, "compatible") and "usbc,sn201202x,spmi" in node.compatible):
        continue
    print(f"Initializing {node._path}...")
    hpm = HpmSPMI(SPMI(u, node._parent._path.removeprefix(u.adt._adt._path)), node.reg[0])
    print(" - mode:", hpm.read(3))
    print(" - version string:", trim_zeros(hpm.read(0x2f)))
    print(" - original power state:", hpm.read(0x20)[0])
    hpm.command(b"SSPS", b"\x00", 0)
    assert hpm.read(0x20)[0] == 0
