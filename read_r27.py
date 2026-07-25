import sys

with open('tools/dolphin/mem1.bin', 'rb') as f:
    f.seek(0x122A400)
    data = f.read(64)

for i in range(0, 64, 16):
    chunk = data[i:i+16]
    hex_str = ' '.join(f'{b:02x}' for b in chunk)
    ascii_str = ''.join(chr(b) if 32 <= b <= 126 else '.' for b in chunk)
    print(f"8122A{0x400 + i:03X}: {hex_str} | {ascii_str}")
