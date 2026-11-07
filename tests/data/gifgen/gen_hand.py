"""Hand-written GIFs with byte-level control (the interlace pass order,
exact delays, the NETSCAPE loop count). Literal-only LZW streams — the
de-encodes exercise the decoder's literal path; PIL's matrix.gif
exercises real compression."""

import struct

class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.cur = 0
        self.n = 0
    def write(self, code, size):
        self.cur |= code << self.n
        self.n += size
        while self.n >= 8:
            self.buf.append(self.cur & 0xFF)
            self.cur >>= 8
            self.n -= 8
    def flush(self):
        if self.n:
            self.buf.append(self.cur & 0xFF)
            self.cur = 0
            self.n = 0
        return bytes(self.buf)

def lzw_literals(indices, min_code=2):
    """Literal-only LZW WITH the code-size growth a decoder applies:
    the table gains one entry per code after the first, and the code
    width bumps when the next free code reaches 2^size."""
    clear = 1 << min_code
    eoi = clear + 1
    size = min_code + 1
    next_code = eoi + 1
    bw = BitWriter()
    bw.write(clear, size)
    prev = None
    for v in indices:
        bw.write(v, size)
        if prev is not None:
            next_code += 1
            if next_code == (1 << size) and size < 12:
                size += 1
        prev = v
    bw.write(eoi, size)
    return bw.flush()

def subblocks(data):
    out = bytearray()
    i = 0
    while i < len(data):
        n = min(255, len(data) - i)
        out.append(n)
        out += data[i:i+n]
        i += n
    out.append(0)
    return bytes(out)

def gif_header(w, h, gct):
    out = bytearray(b"GIF89a")
    out += struct.pack("<HH", w, h)
    out.append(0x80 | ((len(gct) // 3 - 1).bit_length() - 1))
    out.append(0)  # bg
    out.append(0)  # aspect
    out += bytes(gct)
    return bytes(out)

def gce(disposal, delay_cs, has_trans, tidx):
    packed = (disposal << 2) | (1 if has_trans else 0)
    return bytes([0x21, 0xF9, 4, packed, delay_cs & 0xFF,
                  (delay_cs >> 8) & 0xFF, tidx, 0])

def image_desc(left, top, w, h, interlace, lct=None):
    pk = 0x40 if interlace else 0
    if lct is not None:
        pk |= 0x80 | ((len(lct) // 3 - 1).bit_length() - 1)
    out = bytearray([0x2C]) + struct.pack("<HHHH", left, top, w, h)
    out.append(pk)
    if lct is not None:
        out += bytes(lct)
    return bytes(out)

def netscape_loop(count):
    return bytes([0x21, 0xFF, 11]) + b"NETSCAPE2.0" + \
        bytes([3, 1, count & 0xFF, (count >> 8) & 0xFF, 0])

PALETTE = bytes([255,0,0, 0,255,0, 0,0,255, 255,255,0])  # red green blue yellow

# --- interlaced.gif: rows in interlace order, bands ---
w, h = 8, 8
row_color = [(y // 2) % 3 for y in range(h)]
rows_in_order = []
for start, step in ((0,8),(4,8),(2,4),(1,2)):
    y = start
    while y < h:
        rows_in_order.append(y)
        y += step
assert rows_in_order == [0,4,2,6,1,3,5,7], rows_in_order
indices = []
for y in rows_in_order:
    indices += [row_color[y]] * w
data = lzw_literals(indices, 2)
gif = gif_header(w, h, PALETTE) + gce(0, 10, False, 0) + \
    image_desc(0, 0, w, h, True) + bytes([2]) + subblocks(data) + b"\x3B"
open("/home/z/my-project/scripts/gifgen/interlaced.gif", "wb").write(gif)
print(f"interlaced.gif: {len(gif)} bytes")

# --- delays.gif: 3 frames, delays 0cs / 1cs / 2cs, NETSCAPE loop 3 ---
frames = [
    (0, [0]*16),   # delay 0cs -> 100ms floor; red 4x4 full
    (1, [1]*16),   # delay 1cs -> 10ms < 20 -> 100ms floor; green
    (2, [2]*16),   # delay 2cs -> 20ms; blue
]
gif = gif_header(4, 4, PALETTE) + netscape_loop(3)
for delay, idx in frames:
    gif += gce(1, delay, False, 0)
    gif += image_desc(0, 0, 4, 4, False) + bytes([2]) + \
        subblocks(lzw_literals(idx, 2))
gif += b"\x3B"
open("/home/z/my-project/scripts/gifgen/delays.gif", "wb").write(gif)
print(f"delays.gif: {len(gif)} bytes")

# --- emit C arrays ---
for name in ("interlaced", "delays"):
    data = open(f"/home/z/my-project/scripts/gifgen/{name}.gif", "rb").read()
    with open(f"/home/z/my-project/scripts/gifgen/{name}.h", "w") as f:
        f.write(f"/* {name}.gif — hand-written, {len(data)} bytes */\n")
        f.write(f"static const fdk_u8 {name}_gif[] = {{\n")
        for i in range(0, len(data), 12):
            row = ", ".join(f"0x{b:02X}" for b in data[i:i+12])
            f.write(f"    {row},\n")
        f.write("};\n")
        f.write(f"#define {name.upper()}_GIF_LEN {len(data)}u\n\n")
print("arrays emitted")
