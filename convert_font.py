import json

with open("font.json", "r") as f:
    data = json.load(f)

metrics = [{'x':0, 'y':0, 'width':0, 'height':0, 'yOffset':0} for _ in range(256)]

for k, v in data.items():
    idx = v["id"]
    if idx < 256:
        metrics[idx] = v

c_array = "struct { int x, y, width, height, yOffset; } font_metrics[256] = {\n"
for m in metrics:
    c_array += "    { %d, %d, %d, %d, %d },\n" % (m['x'], m['y'], m['width'], m['height'], m['yOffset'])
c_array += "};\n"

with open("src/wii_font_metrics.h", "w") as f:
    f.write(c_array)
