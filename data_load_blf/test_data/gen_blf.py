#!/usr/bin/env python3
"""Regenerate the tiny sample.blf test fixture with python-can (MIT).

    pip install python-can
    python3 gen_blf.py

Writes 3 known CAN frames on two channels (lblf reports channels 1-based):
  ch1 std 0x100  data E8 03 B8 0B 00 00 00 00   @ +0 ms
  ch1 std 0x100  data D0 07 00 00 00 00 00 00   @ +10 ms
  ch2 ext 0x18FEF100 (J1939) data 01..08        @ +20 ms

The committed .blf is checked in (250 bytes); its absolute measurement-start
time is whatever "now" was when generated, so tests assert only on the frame
contents and relative timestamp spacing.
"""
import can

w = can.io.BLFWriter("sample.blf")
frames = [
    can.Message(timestamp=1.000, arbitration_id=0x100, is_extended_id=False, channel=0,
                data=[0xE8, 0x03, 0xB8, 0x0B, 0, 0, 0, 0]),
    can.Message(timestamp=1.010, arbitration_id=0x100, is_extended_id=False, channel=0,
                data=[0xD0, 0x07, 0, 0, 0, 0, 0, 0]),
    can.Message(timestamp=1.020, arbitration_id=0x18FEF100, is_extended_id=True, channel=1,
                data=[1, 2, 3, 4, 5, 6, 7, 8]),
]
for m in frames:
    w.on_message_received(m)
w.stop()
print("wrote sample.blf")
