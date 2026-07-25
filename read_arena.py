import struct

with open('tools/dolphin/mem1.bin', 'rb') as f:
    f.seek(0x3100)
    data = f.read(32)

mem1_lo, mem1_hi = struct.unpack('>II', data[0:8])
mem2_lo, mem2_hi = struct.unpack('>II', data[24:32])

print(f"Dolphin MEM1 Arena: {hex(mem1_lo)} - {hex(mem1_hi)}")
print(f"Dolphin MEM2 Arena: {hex(mem2_lo)} - {hex(mem2_hi)}")
