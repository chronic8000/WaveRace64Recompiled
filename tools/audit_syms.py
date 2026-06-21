import re
from pathlib import Path

text = Path(__file__).resolve().parent.parent / "wr64.us.syms.toml"
text = text.read_text(encoding="utf-8")
funcs = re.findall(r'name = "(codeSEG_[^"]+)", vram = (0x[0-9a-f]+), size = (\d+)', text)
funcs = [(n, int(v, 16), int(s)) for n, v, s in funcs]
funcs.sort(key=lambda x: -x[2])
print("Top 15 codeSEG by size:")
for n, v, s in funcs[:15]:
    print(f"  {n} vram={v:#x} size={s} ({s // 4} insns)")
print("\nLibultra VI region (0x800C6000-0x800C7000):")
for n, v, s in sorted(funcs, key=lambda x: x[1]):
    if 0x800C6000 <= v <= 0x800C7000:
        print(f"  {n} vram={v:#x} size={s} end={v+s:#x}")
