#!/bin/sh
# Iteratively harvest unknown indirect-call targets into extra_funcs.json.
# Each cycle: run lenient -> collect in-text unknown addrs -> regen -> rebuild.
# Stops when a run produces no new targets (or after $1 cycles, default 6).
set -e
export PATH="/c/msys64/mingw64/bin:$PATH"
CYCLES=${1:-6}
cd "$(dirname "$0")"
for i in $(seq 1 $CYCLES); do
    echo "=== harvest cycle $i ==="
    ( cd build && RECOMP_NO_LOGFILE=1 RECOMP_INDIRECT_LENIENT=1 ./robox.exe > ../harvest_run.txt 2>&1 &
      PID=$!; sleep 25; kill $PID 2>/dev/null; true )
    taskkill //IM robox.exe //F 2>/dev/null || true
    sleep 1
    NEW=$(python - <<'EOF'
import json, re
targets = set()
for m in re.finditer(r"unknown addr (0x[0-9a-f]+)", open("harvest_run.txt").read()):
    t = int(m.group(1), 16)
    if 0x80004000 <= t < 0x801c1c80 and (t & 3) == 0:
        targets.add(t)
extra = json.load(open("extra_funcs.json"))
have = {int(e["addr"],16) for e in extra}
new = sorted(targets - have)
for t in new:
    extra.append({"addr": f"0x{t:08x}", "name": f"vt_{t:08x}"})
json.dump(extra, open("extra_funcs.json","w"), indent=1)
print(len(new))
EOF
)
    echo "  new targets: $NEW"
    if [ "$NEW" = "0" ]; then
        echo "  no new targets -- done"
        break
    fi
    sh regen_robox.sh > /dev/null 2>&1
    ninja -C build > /dev/null 2>&1 || ninja -C build 2>&1 | grep -E "error" | head -5
done
echo "=== harvest loop complete ==="
tail -4 harvest_run.txt
