#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import sys, pathlib
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from m1n1.setup import *
from m1n1.shell import run_shell

import os, subprocess
def init_symbols():
    with open('build/m1n1.bin', 'rb') as f:
        f.seek(-32, os.SEEK_END)
        tag_pos = f.tell()
        tag_data = f.read(32)
        assert len(tag_data) == 32
    for i, b in enumerate(tag_data):
        if p.read8(u.base + tag_pos + i) != b:
            print('tag check failed, skipping symbol creation')
            return
    syms = subprocess.run(('nm', 'build/m1n1-raw.elf'), check=True, text=True, capture_output=True)
    for sym in syms.stdout.splitlines():
        addr, kind, name = sym.split(' ')
        addr = u.base + int(addr, 16)
        assert len(kind) == 1 and kind.isalpha()
        globals()[f'sym_{kind}_{name}'] = addr
init_symbols()

aic_base = (aic_dt := u.adt['arm-io/aic']).get_reg(0)[0]
aic_extint = aic_base + aic_dt.extint_baseaddress
aic_maxirq = p.read32(aic_base + aic_dt.maxnumirq_offset) & 0xFFFF
aic_nirq = p.read32(aic_base + aic_dt.cap0_offset) & 0xFFFF
aic_bit_write = lambda n, irq: p.write32(aic_extint + 4*( aic_maxirq + (aic_maxirq//32) * n + irq//32 ), 1<<(irq%32))
aic_sw_set = lambda irq: aic_bit_write(0, irq)
aic_sw_clr = lambda irq: aic_bit_write(1, irq)
aic_mask = lambda irq: aic_bit_write(2, irq)
aic_unmask = lambda irq: aic_bit_write(3, irq)
p.set32(aic_base + 0x14, 1) # config |= enable

def alloc_str(x: str | bytes):
    data = x.encode() if isinstance(x, str) else x
    assert all(data), 'NUL terminators in string'
    ptr = p.malloc(48)
    for i, c in enumerate(data + b'\0'):
        p.write8(ptr + i, c)
    return ptr

spmia0 = SPMI(u, 'arm-io/nub-spmi-a0')
spmia1 = SPMI(u, 'arm-io/nub-spmi-a1')

run_shell(globals(), msg="Have fun!")
