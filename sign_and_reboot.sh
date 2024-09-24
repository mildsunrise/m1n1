#!/bin/bash
set -eEuo pipefail
curl -fO http://192.168.1.147:8000/build/m1n1.bin
echo -en '\x00\x00\x00\x00' >> m1n1.bin
kmutil configure-boot -c m1n1.bin --raw --entry-point 2048 --lowest-virtual-address 0 -v /Volumes/m1n1\ proxy
shutdown -r now
