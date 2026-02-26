#!/usr/bin/env python3
"""
Assemble a minimal CP/M Hello World .COM for the Native8080 emulator.

CP/M .COM files are loaded at 0x0100.
The program uses BDOS function 9 (print '$'-terminated string via CALL 0x0005).

Memory layout after load:
  0x0100  MVI  C, 9          ; BDOS function 9 = print string
  0x0102  LXI  D, 0x010A     ; DE = address of message
  0x0105  CALL 0x0005         ; invoke BDOS
  0x0108  HLT                 ; stop the CPU
  0x0109  NOP                 ; alignment padding (optional)
  0x010A  "Hello, World!$"    ; message (terminated by '$')
"""

ORG = 0x0100          # CP/M load address

# ── Assemble by hand ────────────────────────────────────────────────────────
code = bytearray()

MSG_ADDR = ORG + 10   # 10 bytes of code before the string (see below)

# 0x0100  MVI C, 9         00 001 110 = 0x0E  + immediate 0x09
code += bytes([0x0E, 0x09])                        # 2 bytes  → at 0x0100

# 0x0102  LXI D, MSG_ADDR  00 01 0001 = 0x11  + lb hb
lo = MSG_ADDR & 0xFF
hi = (MSG_ADDR >> 8) & 0xFF
code += bytes([0x11, lo, hi])                      # 3 bytes  → at 0x0102

# 0x0105  CALL 0x0005       0xCD  + lb hb
code += bytes([0xCD, 0x05, 0x00])                  # 3 bytes  → at 0x0105

# 0x0108  HLT               0x76
code += bytes([0x76])                              # 1 byte   → at 0x0108

# 0x0109  NOP (pad to 0x010A so MSG_ADDR arithmetic is exact)
code += bytes([0x00])                              # 1 byte   → at 0x0109

# 0x010A  Message
message = b"Hello, World!$"
code += message

# ── Sanity check ─────────────────────────────────────────────────────────────
assert len(code) == 10 + len(message), "offset mismatch"
print(f"Assembled {len(code)} bytes, load at 0x{ORG:04X}")
print(f"  0x{ORG:04X}  MVI  C, 9")
print(f"  0x{ORG+2:04X}  LXI  D, 0x{MSG_ADDR:04X}   ; -> '{message.decode()}'")
print(f"  0x{ORG+5:04X}  CALL 0x0005")
print(f"  0x{ORG+8:04X}  HLT")
print(f"  0x{MSG_ADDR:04X}  DB   \"{message.decode()}\"")

# ── Write .COM file ──────────────────────────────────────────────────────────
out_path = "hello.com"
with open(out_path, "wb") as f:
    f.write(code)
print(f"\nWrote: {out_path}")
