"""Generates the test GIFs for tests/test_gif.c (C-4 rig).

Frames are 8x8 canvases with hand-placed pixels so the C assertions
can pin exact colors. Saves raw bytes as C arrays to stdout files.

gif A (disposal matrix):
  frame 0: full red, disposal=1 (keep)
  frame 1: green 4x4 at (2,2), disposal=2 (clear), transparent idx 7
  frame 2: fully transparent (all idx 7) -> shows canvas: red ring +
           transparent center (the disposal-2 clear pin)
  frame 3: full blue, disposal=3 (restore previous)
  frame 4: fully transparent -> shows the disposal-3 restore: red
           ring + transparent center again
  delays: 10cs, 5cs, 0cs (floor), 20cs, 1cs (floor)

gif B (interlaced): single frame, interlace=1, gradient rows; pins
  the de-interlace pass order.
"""
from PIL import Image

def frame_draw(size, fn):
    im = Image.new("P", size)
    im.putpalette([
        255,0,0,   # 0 red
        0,255,0,   # 1 green
        0,0,255,   # 2 blue
        255,255,0, # 3 yellow
        0,0,0,     # 4
        0,0,0,     # 5
        0,0,0,     # 6
        0,0,0,     # 7 = transparent
    ])
    px = im.load()
    fn(px, im)
    return im

# --- gif A: the disposal matrix ---
A = []
def a0(px, im):
    for y in range(8):
        for x in range(8):
            px[x, y] = 0
A.append(frame_draw((8,8), a0))

def a1(px, im):
    for y in range(2,6):
        for x in range(2,6):
            px[x, y] = 1
A.append(frame_draw((8,8), a1))

def a2(px, im):
    for y in range(8):
        for x in range(8):
            px[x, y] = 7  # transparent everywhere
A.append(frame_draw((8,8), a2))

def a3(px, im):
    for y in range(8):
        for x in range(8):
            px[x, y] = 2
A.append(frame_draw((8,8), a3))
A.append(frame_draw((8,8), a2))  # a4 = transparent

delays = [10, 5, 0, 20, 1]
disposals = [1, 2, 0, 3, 0]
imA = A[0].copy()
imA.save("/home/z/my-project/scripts/gifgen/matrix.gif", save_all=True,
         append_images=A[1:], duration=delays, disposal=disposals,
         transparency=7, loop=0)

# --- gif B: interlaced gradient ---
def b0(px, im):
    for y in range(8):
        for x in range(8):
            px[x, y] = (y // 2) % 3   # row bands: 0,0,1,1,2,2,0,0
B = frame_draw((8,8), b0)
# PIL writes interlace when saving with interlace=True? For P mode GIF:
B.save("/home/z/my-project/scripts/gifgen/interlaced.gif", interlace=True)

# --- gif C: single frame, no animation ---
C = frame_draw((4,4), lambda px, im: [px.__setitem__((x, y), 3)
          for y in range(4) for x in range(4)])
C.save("/home/z/my-project/scripts/gifgen/single.gif")

for name in ("matrix", "interlaced", "single"):
    data = open(f"/home/z/my-project/scripts/gifgen/{name}.gif", "rb").read()
    print(f"{name}: {len(data)} bytes")

# Emit C arrays
import sys
def emit(name, path, out):
    data = open(path, "rb").read()
    with open(out, "w") as f:
        f.write(f"/* {name}.gif — PIL-generated, {len(data)} bytes */\n")
        f.write(f"static const fdk_u8 {name}_gif[] = {{\n")
        for i in range(0, len(data), 12):
            row = ", ".join(f"0x{b:02X}" for b in data[i:i+12])
            f.write(f"    {row},\n")
        f.write("};\n")
        f.write(f"#define {name.upper()}_GIF_LEN {len(data)}u\n\n")

emit("matrix", "/home/z/my-project/scripts/gifgen/matrix.gif",
     "/home/z/my-project/scripts/gifgen/matrix.h")
emit("interlaced", "/home/z/my-project/scripts/gifgen/interlaced.gif",
     "/home/z/my-project/scripts/gifgen/interlaced.h")
emit("single", "/home/z/my-project/scripts/gifgen/single.gif",
     "/home/z/my-project/scripts/gifgen/single.h")
print("arrays emitted")
