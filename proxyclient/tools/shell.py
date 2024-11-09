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

run_shell(globals(), msg="Have fun!")
