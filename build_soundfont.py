import struct, math, os, sys, glob

def find_files():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    wt_path = pcm_path = None

    for root, dirs, files in os.walk(script_dir):
        for f in files:
            fl = f.lower()
            if fl == 'robox.wt' and wt_path is None:
                wt_path = os.path.join(root, f)
            elif fl == 'robox.pcm' and pcm_path is None:
                pcm_path = os.path.join(root, f)
            if wt_path and pcm_path:
                return wt_path, pcm_path

    return wt_path, pcm_path

def ask_file(title, filetypes):
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk()
        root.withdraw()
        path = filedialog.askopenfilename(title=title, filetypes=filetypes)
        root.destroy()
        return path if path else None
    except:
        return None

def get_input_files():
    if len(sys.argv) >= 3:
        return sys.argv[1], sys.argv[2]

    print("Searching for robox.wt and robox.pcm...")
    wt_path, pcm_path = find_files()

    if wt_path:
        print(f"  Found: {wt_path}")
    if pcm_path:
        print(f"  Found: {pcm_path}")

    if not wt_path:
        print("  robox.wt not found, opening file dialog...")
        wt_path = ask_file("Select robox.wt", [("WT files", "*.wt"), ("All files", "*.*")])
        if not wt_path:
            print("Error: robox.wt not selected")
            sys.exit(1)

    if not pcm_path:
        print("  robox.pcm not found, opening file dialog...")
        pcm_path = ask_file("Select robox.pcm", [("PCM files", "*.pcm"), ("All files", "*.*")])
        if not pcm_path:
            print("Error: robox.pcm not selected")
            sys.exit(1)

    return wt_path, pcm_path

def parse_wt(wt_path):
    with open(wt_path, 'rb') as f:
        wt = f.read()
    offsets = []
    for i in range(0, len(wt), 4):
        v = struct.unpack('>I', wt[i:i+4])[0]
        if v == 0xFFFFFFFF: break
        offsets.append(v)
    sec_keymap = wt[offsets[1]:offsets[2]]
    sec_desc   = wt[offsets[2]:offsets[3]]
    sec_env    = wt[offsets[3]:offsets[4]]
    sec_pcmtbl = wt[offsets[4]:offsets[5]]

    num_pcm = len(sec_pcmtbl) // 16
    pcm_entries = []
    for i in range(num_pcm):
        e = sec_pcmtbl[i*16:(i+1)*16]
        re, off, ln, fl = struct.unpack('>IIII', e)
        pcm_entries.append({'rate': re & 0xFFFF, 'offset': off, 'length': ln})

    num_desc = len(sec_desc) // 24
    descs = []
    for i in range(num_desc):
        e = sec_desc[i*24:(i+1)*24]
        descs.append({
            'root_key': e[0], 'flags': e[1],
            'fine_tune': struct.unpack('>h', e[2:4])[0],
            'pitch': struct.unpack('>i', e[4:8])[0],
            'loop_start': struct.unpack('>I', e[8:12])[0],
            'loop_len': struct.unpack('>I', e[12:16])[0],
            'inst_type': struct.unpack('>I', e[16:20])[0],
            'pcm_idx': struct.unpack('>I', e[20:24])[0],
        })

    num_env = len(sec_env) // 80
    env_release_tc = {}
    for ei in range(num_env):
        e = sec_env[ei*80:(ei+1)*80]
        w9 = abs(struct.unpack('>i', e[36:40])[0])
        if w9 < 1: w9 = 1
        log_val = math.log(w9)
        ms = 1500 - (log_val - 10.3) * (1450 / 3.4)
        ms = max(30, min(2000, ms))
        env_release_tc[ei] = int(1200 * math.log2(ms / 1000))

    def get_keymap(prog, note):
        v = struct.unpack('>H', sec_keymap[prog*256+note*2:prog*256+note*2+2])[0]
        return v if v != 0xFFFF else None

    return pcm_entries, descs, get_keymap, num_pcm, num_desc, env_release_tc

def convert_8to16(raw_bytes):
    out = bytearray()
    for b in raw_bytes:
        out.extend(struct.pack('<h', struct.unpack('b', bytes([b]))[0] * 256))
    return bytes(out)

def build_all(wt_path, pcm_path, sf2_path, dls_path):
    pcm_entries, descs, get_keymap, num_pcm, num_desc, env_release_tc = parse_wt(wt_path)
    with open(pcm_path, 'rb') as f:
        pcm_raw = f.read()

    decoded = {}
    def get_decoded(pi):
        if pi not in decoded:
            pe = pcm_entries[pi]
            decoded[pi] = convert_8to16(pcm_raw[pe['offset']:pe['offset']+pe['length']])
        return decoded[pi]

    instruments = []
    for prog in range(128):
        zones = []
        n = 0
        while n < 128:
            di = get_keymap(prog, n)
            if di is None: n += 1; continue
            hi = n
            while hi+1 < 128 and get_keymap(prog, hi+1) == di: hi += 1
            if di >= num_desc: n = hi+1; continue
            d = descs[di]
            pi = d['pcm_idx']
            if pi >= num_pcm: n = hi+1; continue
            get_decoded(pi)
            ls, ll = d['loop_start'], d['loop_len']
            has_loop = ls > 0 and ll > 0
            env_idx = d['inst_type']
            release_tc = env_release_tc.get(env_idx, 0)
            zones.append({
                'key_lo': n, 'key_hi': hi, 'pcm_idx': pi,
                'root_key': d['root_key'], 'fine_tune': d['fine_tune'],
                'loop_start': ls, 'loop_len': ll, 'has_loop': has_loop,
                'is_drum': (prog == 39), 'release_tc': release_tc,
            })
            n = hi + 1
        if zones:
            is_drum = (prog == 39)
            name = 'Drums' if is_drum else f'Prog{prog:03d}'
            instruments.append({
                'name': name,
                'prog': 0 if is_drum else prog,
                'bank': 128 if is_drum else 0,
                'is_drum': is_drum, 'zones': zones,
                'release_tc': zones[0]['release_tc'],
                'orig_prog': prog,
            })

    print(f"Built {len(instruments)} instruments, {len(decoded)} unique PCM samples")
    for inst in instruments:
        ms = 1000 * 2**(inst['release_tc']/1200)
        print(f"  Prog {inst['orig_prog']:2d}: {inst['name']:20s} ({len(inst['zones']):2d} zones, release={ms:.0f}ms)")

    build_sf2(instruments, decoded, pcm_entries, descs, sf2_path)
    build_dls(instruments, decoded, pcm_entries, descs, dls_path)

def build_sf2(instruments, decoded, pcm_entries, descs, output_path):
    PADDING = 46
    sorted_pi = sorted(decoded.keys())
    pi_to_si = {pi: i for i, pi in enumerate(sorted_pi)}

    smpl = bytearray()
    sample_info = {}
    for si, pi in enumerate(sorted_pi):
        start = len(smpl) // 2
        smpl.extend(decoded[pi])
        end = len(smpl) // 2
        smpl.extend(b'\x00' * (PADDING * 2))
        sample_info[si] = (start, end)

    shdr = bytearray()
    shdr_loops = {}
    for si, pi in enumerate(sorted_pi):
        pe = pcm_entries[pi]
        start, end = sample_info[si]
        nsamp = end - start
        loop_s, loop_e = start, end
        for d in descs:
            if d['pcm_idx'] == pi and d['loop_start'] > 0 and d['loop_len'] > 0:
                ls2, le2 = d['loop_start'], d['loop_start'] + d['loop_len']
                if le2 <= nsamp:
                    loop_s, loop_e = start + ls2, start + le2
                break
        shdr_loops[si] = (loop_s, loop_e)
        name = f'Smp{pi:03d}'.encode().ljust(20, b'\x00')[:20]
        shdr.extend(name)
        shdr.extend(struct.pack('<IIIII', start, end, loop_s, loop_e, pe['rate']))
        shdr.extend(struct.pack('<bbHH', 60, 0, 0, 1))
    shdr.extend(b'EOS\x00'.ljust(20, b'\x00'))
    shdr.extend(struct.pack('<IIIIIbbHH', 0,0,0,0,0,0,0,0,0))

    inst_b = bytearray(); ibag_b = bytearray(); igen_b = bytearray()
    igen_i = 0; ibag_i = 0
    for sf2i in instruments:
        inst_b.extend(sf2i['name'].encode().ljust(20, b'\x00')[:20])
        inst_b.extend(struct.pack('<H', ibag_i))
        for z in sf2i['zones']:
            ibag_b.extend(struct.pack('<HH', igen_i, 0))
            si = pi_to_si[z['pcm_idx']]
            igen_b.extend(struct.pack('<HBB', 43, z['key_lo'], z['key_hi'])); igen_i += 1
            igen_b.extend(struct.pack('<Hh', 58, z['root_key'])); igen_i += 1
            ft = max(-99, min(99, z['fine_tune']))
            if ft != 0:
                igen_b.extend(struct.pack('<Hh', 52, ft)); igen_i += 1
            if z['has_loop']:
                igen_b.extend(struct.pack('<Hh', 54, 1)); igen_i += 1
                s_start = sample_info[si][0]
                for gen_id, desired, default in [
                    (4, s_start + z['loop_start'], shdr_loops[si][0]),
                    (12, s_start + z['loop_start'] + z['loop_len'], shdr_loops[si][1])
                ]:
                    diff = desired - default
                    if diff != 0:
                        fine = diff % 32768 if diff > 0 else -((-diff) % 32768)
                        coarse = diff // 32768 if diff > 0 else -((-diff) // 32768)
                        if fine:
                            igen_b.extend(struct.pack('<Hh', gen_id, fine)); igen_i += 1
                        if coarse:
                            igen_b.extend(struct.pack('<Hh', 45 if gen_id == 4 else 50, coarse)); igen_i += 1
            else:
                igen_b.extend(struct.pack('<Hh', 54, 0)); igen_i += 1
            rtc = max(-12000, min(8000, z.get('release_tc', 0)))
            igen_b.extend(struct.pack('<Hh', 38, rtc)); igen_i += 1
            igen_b.extend(struct.pack('<Hh', 53, si)); igen_i += 1
            ibag_i += 1

    inst_b.extend(b'EOI\x00'.ljust(20, b'\x00'))
    inst_b.extend(struct.pack('<H', ibag_i))
    ibag_b.extend(struct.pack('<HH', igen_i, 0))
    igen_b.extend(struct.pack('<Hh', 0, 0))

    phdr_b = bytearray(); pbag_b = bytearray(); pgen_b = bytearray()
    pgen_i = 0; pbag_i = 0
    for idx, sf2i in enumerate(instruments):
        phdr_b.extend(sf2i['name'].encode().ljust(20, b'\x00')[:20])
        phdr_b.extend(struct.pack('<HHH', sf2i['prog'], sf2i['bank'], pbag_i))
        phdr_b.extend(struct.pack('<III', 0, 0, 0))
        pbag_b.extend(struct.pack('<HH', pgen_i, 0))
        pgen_b.extend(struct.pack('<Hh', 41, idx)); pgen_i += 1; pbag_i += 1

    drum_idx = next((i for i, x in enumerate(instruments) if x['is_drum']), None)
    if drum_idx is not None:
        phdr_b.extend(b'DrumKit039\x00'.ljust(20, b'\x00')[:20])
        phdr_b.extend(struct.pack('<HHH', 39, 0, pbag_i))
        phdr_b.extend(struct.pack('<III', 0, 0, 0))
        pbag_b.extend(struct.pack('<HH', pgen_i, 0))
        pgen_b.extend(struct.pack('<Hh', 41, drum_idx)); pgen_i += 1; pbag_i += 1

    phdr_b.extend(b'EOP\x00'.ljust(20, b'\x00'))
    phdr_b.extend(struct.pack('<HHH', 0, 0, pbag_i))
    phdr_b.extend(struct.pack('<III', 0, 0, 0))
    pbag_b.extend(struct.pack('<HH', pgen_i, 0))
    pgen_b.extend(struct.pack('<Hh', 0, 0))

    def chunk(tag, data):
        d = bytes(data)
        if len(d) % 2: d += b'\x00'
        return tag.encode() + struct.pack('<I', len(d)) + d
    def lst(tag, *chunks):
        inner = tag.encode() + b''.join(chunks)
        return b'LIST' + struct.pack('<I', len(inner)) + inner

    info = lst('INFO',
        chunk('ifil', struct.pack('<HH', 2, 1)),
        chunk('isng', b'EMU8000\x00'),
        chunk('INAM', b'Robox Soundfont\x00'))
    sdta = lst('sdta', chunk('smpl', bytes(smpl)))
    pdta = lst('pdta',
        chunk('phdr', bytes(phdr_b)), chunk('pbag', bytes(pbag_b)),
        chunk('pmod', struct.pack('<HHHHH', 0,0,0,0,0)),
        chunk('pgen', bytes(pgen_b)),
        chunk('inst', bytes(inst_b)), chunk('ibag', bytes(ibag_b)),
        chunk('imod', struct.pack('<HHHHH', 0,0,0,0,0)),
        chunk('igen', bytes(igen_b)), chunk('shdr', bytes(shdr)))

    riff_inner = b'sfbk' + info + sdta + pdta
    riff = b'RIFF' + struct.pack('<I', len(riff_inner)) + riff_inner
    with open(output_path, 'wb') as f:
        f.write(riff)
    print(f"SF2: {output_path} ({len(riff):,} bytes)")

def build_dls(instruments, decoded, pcm_entries, descs, output_path):
    sorted_pi = sorted(decoded.keys())
    pi_to_si = {pi: i for i, pi in enumerate(sorted_pi)}

    wvpl_inner = bytearray()
    wave_offsets = {}

    for si, pi in enumerate(sorted_pi):
        pe = pcm_entries[pi]
        data = decoded[pi]

        def_ls = def_ll = 0; def_root = 60; def_ft = 0
        for d in descs:
            if d['pcm_idx'] == pi:
                def_root = d['root_key']
                def_ft = d['fine_tune']
                if d['loop_start'] > 0 and d['loop_len'] > 0:
                    def_ls, def_ll = d['loop_start'], d['loop_len']
                break
        has_loop = def_ls > 0 and def_ll > 0

        wav = bytearray()
        fmt_data = struct.pack('<HHIIHH', 1, 1, pe['rate'], pe['rate']*2, 2, 16)
        fmt_data += struct.pack('<H', 0)
        wav.extend(b'fmt '); wav.extend(struct.pack('<I', len(fmt_data))); wav.extend(fmt_data)

        n_loops = 1 if has_loop else 0
        wsmp = struct.pack('<IHhiII', 20, def_root,
            max(-32768, min(32767, def_ft)), 0, 0, n_loops)
        if has_loop:
            wsmp += struct.pack('<IIII', 16, 0, def_ls, def_ll)
        wav.extend(b'wsmp'); wav.extend(struct.pack('<I', len(wsmp))); wav.extend(wsmp)

        wav.extend(b'data'); wav.extend(struct.pack('<I', len(data))); wav.extend(data)
        if len(data) % 2: wav.append(0)

        wave_list = b'wave' + bytes(wav)
        wave_entry = b'LIST' + struct.pack('<I', len(wave_list)) + wave_list
        wave_offsets[si] = len(wvpl_inner)
        wvpl_inner.extend(wave_entry)

    wvpl = b'LIST' + struct.pack('<I', 4 + len(wvpl_inner)) + b'wvpl' + bytes(wvpl_inner)

    n_waves = len(sorted_pi)
    ptbl_data = struct.pack('<II', 8, n_waves)
    for si in range(n_waves):
        ptbl_data += struct.pack('<I', wave_offsets[si])
    ptbl = b'ptbl' + struct.pack('<I', len(ptbl_data)) + ptbl_data

    def make_art1(release_tc):
        release_tc = max(-12000, min(8000, release_tc))
        conn_lfo_freq = struct.pack('<HHHHi', 0, 0, 0x0104, 0, 58272)
        conn_vibrato  = struct.pack('<HHHHi', 0x0001, 0x0081, 0x0003, 0, 3276800)
        conn_release  = struct.pack('<HHHHi', 0, 0, 0x0209, 0, release_tc)
        art1_data = struct.pack('<II', 8, 3) + conn_lfo_freq + conn_vibrato + conn_release
        art1_chunk = b'art1' + struct.pack('<I', len(art1_data)) + art1_data
        return b'LIST' + struct.pack('<I', 4 + len(art1_chunk)) + b'lart' + art1_chunk

    def make_ins_info(name):
        inam = name.encode('ascii') + b'\x00'
        if len(inam) % 2: inam += b'\x00'
        info_inner = b'INFO' + b'INAM' + struct.pack('<I', len(inam)) + inam
        return b'LIST' + struct.pack('<I', len(info_inner)) + info_inner

    dls_instruments = []
    drum_inst = next((inst for inst in instruments if inst['is_drum']), None)

    for inst in instruments:
        if inst['is_drum']:
            dls_instruments.append({
                'name': inst['name'], 'prog': 39, 'bank': 0,
                'ulBank': 0x00000000, 'zones': inst['zones'],
                'release_tc': inst['release_tc'],
            })
        else:
            dls_instruments.append({
                'name': inst['name'], 'prog': inst['prog'], 'bank': 0,
                'ulBank': 0x00000000, 'zones': inst['zones'],
                'release_tc': inst['release_tc'],
            })

    if drum_inst:
        dls_instruments.append({
            'name': 'Drum Kit', 'prog': 0, 'bank': 128,
            'ulBank': 0x80000000, 'zones': drum_inst['zones'],
            'release_tc': drum_inst['release_tc'],
        })

    lins_inner = bytearray()
    for inst in dls_instruments:
        ins_inner = bytearray()

        insh = struct.pack('<III', len(inst['zones']), inst['ulBank'], inst['prog'])
        ins_inner.extend(b'insh'); ins_inner.extend(struct.pack('<I', len(insh))); ins_inner.extend(insh)

        ins_inner.extend(make_art1(inst['release_tc']))

        lrgn_inner = bytearray()
        for z in inst['zones']:
            rgn_inner = bytearray()

            rgnh = struct.pack('<HHHHHH', z['key_lo'], z['key_hi'], 0, 127, 0, 0)
            rgn_inner.extend(b'rgnh'); rgn_inner.extend(struct.pack('<I', len(rgnh))); rgn_inner.extend(rgnh)

            has_loop = z['has_loop']
            n_loops = 1 if has_loop else 0
            wsmp = struct.pack('<IHhiII', 20, z['root_key'],
                max(-32768, min(32767, z['fine_tune'])), 0, 0, n_loops)
            if has_loop:
                wsmp += struct.pack('<IIII', 16, 0, z['loop_start'], z['loop_len'])
            rgn_inner.extend(b'wsmp'); rgn_inner.extend(struct.pack('<I', len(wsmp))); rgn_inner.extend(wsmp)

            si = pi_to_si[z['pcm_idx']]
            wlnk = struct.pack('<HHII', 0, 0, 1, si)
            rgn_inner.extend(b'wlnk'); rgn_inner.extend(struct.pack('<I', len(wlnk))); rgn_inner.extend(wlnk)

            rgn_list = b'rgn ' + bytes(rgn_inner)
            lrgn_inner.extend(b'LIST' + struct.pack('<I', len(rgn_list)) + rgn_list)

        lrgn = b'lrgn' + bytes(lrgn_inner)
        ins_inner.extend(b'LIST' + struct.pack('<I', len(lrgn)) + lrgn)

        ins_inner.extend(make_ins_info(inst['name']))

        ins_list = b'ins ' + bytes(ins_inner)
        lins_inner.extend(b'LIST' + struct.pack('<I', len(ins_list)) + ins_list)

    lins = b'LIST' + struct.pack('<I', 4 + len(lins_inner)) + b'lins' + bytes(lins_inner)

    colh = b'colh' + struct.pack('<II', 4, len(dls_instruments))

    dls_inner = b'DLS ' + colh + lins + ptbl + wvpl
    dls = b'RIFF' + struct.pack('<I', len(dls_inner)) + dls_inner

    with open(output_path, 'wb') as f:
        f.write(dls)
    print(f"DLS: {output_path} ({len(dls):,} bytes)")

if __name__ == '__main__':
    wt_path, pcm_path = get_input_files()

    out_dir = os.path.dirname(wt_path)
    sf2_out = os.path.join(out_dir, 'robox.sf2')
    dls_out = os.path.join(out_dir, 'robox.dls')

    if len(sys.argv) >= 4:
        sf2_out = sys.argv[3]
    if len(sys.argv) >= 5:
        dls_out = sys.argv[4]

    build_all(wt_path, pcm_path, sf2_out, dls_out)
