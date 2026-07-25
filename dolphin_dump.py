import dolphin_memory_engine

def dump_routing_data(r3_val):
    if not dolphin_memory_engine.is_hooked():
        print("Hooking to Dolphin...")
        dolphin_memory_engine.hook()
        if not dolphin_memory_engine.is_hooked():
            print("Failed to hook to Dolphin. Make sure Dolphin is running with the game paused at the breakpoint!")
            return

    print(f"Using r3 = 0x{r3_val:08X}")
    
    # r5 = MEM_R32(r3 + 0x1100)
    # read_word automatically handles Wii's big-endian memory and gives us the correct integer
    r5 = dolphin_memory_engine.read_word(r3_val + 0x1100)
    print(f"Read r5 (base pointer) = 0x{r5:08X}")

    if r5 == 0 or r5 < 0x80000000 or r5 > 0x81800000:
        print("r5 looks invalid. Are you sure Dolphin is paused at the breakpoint and r3 is correct?")
        return

    # The array of nodes starts at r5 + 4
    array_base = r5 + 4
    
    print("\n--- Routing Tree Dump ---")
    # Let's dump the first 10 nodes to see if initialization cleared outId to -1
    for i in range(10):
        node_addr = array_base + (i * 0x2C)
        
        # The outId is the very first 32-bit integer in the 0x2C struct
        outId = dolphin_memory_engine.read_word(node_addr)
        
        if outId == 0xFFFFFFFF:
            outId_str = "-1 (Safe/Leaf)"
        else:
            outId_str = f"{outId}"
            
        print(f"Group {i} (addr 0x{node_addr:08X}): outId = {outId_str}")

    print("\nDone!")

if __name__ == '__main__':
    # !!! IMPORTANT !!!
    # When Dolphin pauses at 0x80246BC4, look at the Registers window.
    # Copy the hex value of r3 and paste it here:
    R3_VALUE = 0x81234567 
    
    dump_routing_data(R3_VALUE)
