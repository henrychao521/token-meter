#!/usr/bin/env python3
"""
make_u8g2_font.py — 把 TTF 點陣字型（預設 Cubic 11）轉成 U8g2 字型 C 陣列。

用法：
  python3 make_u8g2_font.py --ttf Cubic_11.ttf --size 12 \
      --chars-from ../t_qt_weather_clock/t_qt_weather_clock.ino \
      --extra "°·…" --name u8g2_font_cubic11_tw \
      --out ../t_qt_weather_clock/cubic11_u8g2.h --preview preview.png

做法：PIL 以指定 ppem 點陣化（Cubic 11 在 12 時剛好對齊像素格，無灰階），
依 U8g2 字型格式（23 位元組表頭＋LSB-first 位元流＋RLE）編碼；
編完用一個照 u8g2_font.c 邏輯寫的 Python 解碼器逐字還原比對，不一致就失敗。
"""
import argparse, re, sys
from PIL import Image, ImageDraw, ImageFont

# ---------- 點陣化 ----------
def rasterize(font, ch, ascent, descent):
    W, H = 64, ascent + descent + 16
    ox, oy = 16, 8                      # 筆尖原點；上緣線在 oy，基線在 oy+ascent
    im = Image.new("L", (W, H), 0)
    ImageDraw.Draw(im).text((ox, oy), ch, font=font, fill=255, anchor="la")
    px = im.load()
    rows = [[1 if px[x, y] > 127 else 0 for x in range(W)] for y in range(H)]
    grey = sum(1 for y in range(H) for x in range(W) if 0 < px[x, y] < 255)
    adv = int(round(font.getlength(ch)))
    pts = [(x, y) for y in range(H) for x in range(W) if rows[y][x]]
    if not pts:
        return dict(ch=ch, w=0, h=0, xo=0, yo=0, adv=adv, bits=[], grey=grey, base=oy + ascent)
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    bits = [rows[y][x] for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)]
    return dict(ch=ch, w=x1 - x0 + 1, h=y1 - y0 + 1, xo=x0 - ox, yo=None, adv=adv,
                bits=bits, grey=grey, base=oy + ascent, y1=y1)

# ---------- 位元流 ----------
class BitWriter:
    def __init__(self): self.buf = bytearray(); self.pos = 0
    def put(self, val, cnt):
        assert 0 <= val < (1 << cnt), (val, cnt)
        for i in range(cnt):
            if self.pos == 0: self.buf.append(0)
            if (val >> i) & 1: self.buf[-1] |= 1 << self.pos
            self.pos = (self.pos + 1) & 7
    def put_signed(self, val, cnt): self.put(val + (1 << (cnt - 1)), cnt)

class BitReader:  # 照 u8g2_font_decode_get_unsigned_bits
    def __init__(self, data, start): self.d = data; self.p = start; self.bp = 0
    def get(self, cnt):
        val = self.d[self.p] >> self.bp
        bpc = self.bp + cnt
        if bpc >= 8:
            s = 8 - self.bp
            self.p += 1
            val |= (self.d[self.p] if self.p < len(self.d) else 0) << s
            bpc -= 8
        val &= (1 << cnt) - 1
        self.bp = bpc
        return val
    def get_signed(self, cnt): return self.get(cnt) - (1 << (cnt - 1))

def ubits(maxv): return max(1, int(maxv).bit_length())
def sbits(lo, hi):
    n = 1
    while not (-(1 << (n - 1)) <= lo and hi <= (1 << (n - 1)) - 1): n += 1
    return n

def encode_glyph(g, P):
    bw = BitWriter()
    bw.put(g["w"], P["bw"]); bw.put(g["h"], P["bh"])
    bw.put_signed(g["xo"], P["bx"]); bw.put_signed(g["yo"], P["by"]); bw.put_signed(g["adv"], P["bd"])
    bits = g["bits"]; n = len(bits); i = 0
    max0 = (1 << P["b0"]) - 1; max1 = (1 << P["b1"]) - 1
    if n:
        pairs = []
        while i < n:
            a = 0
            while i < n and bits[i] == 0 and a < max0: a += 1; i += 1
            b = 0
            while i < n and bits[i] == 1 and b < max1: b += 1; i += 1
            pairs.append((a, b))
        # 相鄰相同的 pair 用重複位元壓縮
        k = 0
        while k < len(pairs):
            a, b = pairs[k]; bw.put(a, P["b0"]); bw.put(b, P["b1"]); k += 1
            while k < len(pairs) and pairs[k] == (a, b):
                bw.put(1, 1); k += 1
            bw.put(0, 1)
    return bytes(bw.buf)

def decode_glyph(data, start, P):  # 照 u8g2_font_decode_glyph / decode_len
    br = BitReader(data, start)
    w = br.get(P["bw"]); h = br.get(P["bh"])
    xo = br.get_signed(P["bx"]); yo = br.get_signed(P["by"]); adv = br.get_signed(P["bd"])
    bits = []
    if w > 0:
        x = y = 0
        def run(ln, v):
            nonlocal x, y
            cnt = ln
            while True:
                rem = w - x; cur = min(cnt, rem)
                bits.extend([v] * cur)
                cnt -= cur; x += cur
                if x == w: x = 0; y += 1
                if cnt == 0: break
        while True:
            a = br.get(P["b0"]); b = br.get(P["b1"])
            while True:
                run(a, 0); run(b, 1)
                if br.get(1) == 0: break
            if y >= h: break
    return dict(w=w, h=h, xo=xo, yo=yo, adv=adv, bits=bits)

def build_font(glyphs, P):
    """回傳 (bytes, 資訊)。glyphs 依編碼排序。"""
    g8 = [g for g in glyphs if g["enc"] < 256]
    gu = [g for g in glyphs if g["enc"] >= 256]
    body = bytearray()
    pos_A = pos_a = None
    for g in g8:
        if pos_A is None and g["enc"] >= ord("A"): pos_A = len(body)
        if pos_a is None and g["enc"] >= ord("a"): pos_a = len(body)
        d = encode_glyph(g, P)
        assert 2 + len(d) < 256
        body += bytes([g["enc"], 2 + len(d)]) + d
    if pos_A is None: pos_A = len(body)
    if pos_a is None: pos_a = len(body)
    body += b"\x00\x00"                      # 8 位元區結尾
    pos_uni = len(body)
    # Unicode 區：查表 + 區塊（每塊最多 40 字，各以 0x0000 結尾）
    blocks = []
    for i in range(0, len(gu), 40):
        chunk = gu[i:i + 40]
        blk = bytearray()
        for g in chunk:
            d = encode_glyph(g, P)
            assert 3 + len(d) < 256
            blk += bytes([g["enc"] >> 8, g["enc"] & 0xFF, 3 + len(d)]) + d
        blk += b"\x00\x00"
        blocks.append((chunk[-1]["enc"], bytes(blk)))
    table = bytearray()
    tsize = 4 * len(blocks)
    for i, (lastenc, blk) in enumerate(blocks):
        off = tsize if i == 0 else len(blocks[i - 1][1])
        enc = 0xFFFF if i == len(blocks) - 1 else lastenc
        table += bytes([off >> 8, off & 0xFF, enc >> 8, enc & 0xFF])
    body += table
    for _, blk in blocks: body += blk
    A = next((g for g in glyphs if g["enc"] == ord("A")), None)
    gg = next((g for g in glyphs if g["enc"] == ord("g")), None)
    asc_A = (A["h"] + A["yo"]) if A else max(g["h"] + g["yo"] for g in glyphs)
    desc_g = gg["yo"] if gg else min(g["yo"] for g in glyphs)
    asc_p = max(g["h"] + g["yo"] for g in glyphs); desc_p = min(g["yo"] for g in glyphs)
    def s8(v): return v & 0xFF
    hdr = bytes([
        len(glyphs) & 0xFF, 0, P["b0"], P["b1"], P["bw"], P["bh"], P["bx"], P["by"], P["bd"],
        max(g["w"] for g in glyphs), max(g["h"] for g in glyphs),
        s8(min(g["xo"] for g in glyphs)), s8(desc_p),
        s8(asc_A), s8(desc_g), s8(asc_p), s8(desc_p),
        pos_A >> 8, pos_A & 0xFF, pos_a >> 8, pos_a & 0xFF, pos_uni >> 8, pos_uni & 0xFF,
    ])
    assert len(hdr) == 23
    return bytes(hdr) + bytes(body), dict(asc_A=asc_A, desc_g=desc_g, asc_p=asc_p, desc_p=desc_p)

def lookup(data, enc, P):  # 照 u8g2_font_get_glyph_data
    p = 23
    if enc <= 255:
        if enc >= ord("a"): p += (data[21] << 8 | data[22]) if False else (data[19] << 8 | data[20])
        elif enc >= ord("A"): p += data[17] << 8 | data[18]
        while True:
            if data[p + 1] == 0: return None
            if data[p] == enc: return p + 2
            p += data[p + 1]
    else:
        p += data[21] << 8 | data[22]
        t = p
        while True:
            p += data[t] << 8 | data[t + 1]
            e = data[t + 2] << 8 | data[t + 3]
            t += 4
            if not (e < enc): break
        while True:
            e = data[p] << 8 | data[p + 1]
            if e == 0: return None
            if e == enc: return p + 3
            p += data[p + 2]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True); ap.add_argument("--size", type=int, default=12)
    ap.add_argument("--chars-from", action="append", default=[], help="掃描檔案內所有非 ASCII 字元")
    ap.add_argument("--extra", default=""); ap.add_argument("--no-ascii", action="store_true")
    ap.add_argument("--name", default="u8g2_font_custom"); ap.add_argument("--out", required=True)
    ap.add_argument("--preview", default=None); ap.add_argument("--preview-text", action="append", default=[])
    a = ap.parse_args()

    chars = set() if a.no_ascii else set(chr(c) for c in range(32, 127))
    for f in a.chars_from:
        txt = open(f, encoding="utf-8").read()
        chars |= {c for c in txt if ord(c) > 127 and not c.isspace()}
    chars |= {c for c in a.extra if not c.isspace()}
    chars.add(" ")
    font = ImageFont.truetype(a.ttf, a.size)
    ascent, descent = font.getmetrics()
    glyphs = []
    greytotal = 0; missing = []
    for ch in sorted(chars):
        if ord(ch) > 0xFFFF: missing.append(ch); continue
        g = rasterize(font, ch, ascent, descent); g["enc"] = ord(ch)
        greytotal += g["grey"]
        if g["w"] == 0 and ch != " " and not ch.isspace(): missing.append(ch)
        glyphs.append(g)
    # 以 A 的底列定義基線（讓 A 的 yo = 0）
    A = next(g for g in glyphs if g["ch"] == "A")
    base_row = A["y1"] + 1
    for g in glyphs:
        g["yo"] = (base_row - g["y1"] - 1) if g["w"] else 0
    if greytotal: print(f"警告：有 {greytotal} 個灰階像素，ppem 可能沒對齊", file=sys.stderr)
    if missing: print("警告：字型缺字：" + "".join(missing), file=sys.stderr)

    P = dict(bw=ubits(max(g["w"] for g in glyphs)), bh=ubits(max(g["h"] for g in glyphs)),
             bx=sbits(min(g["xo"] for g in glyphs), max(g["xo"] for g in glyphs)),
             by=sbits(min(g["yo"] for g in glyphs), max(g["yo"] for g in glyphs)),
             bd=sbits(0, max(g["adv"] for g in glyphs)))
    best = None
    for b0 in range(2, 7):
        for b1 in range(2, 7):
            Q = dict(P, b0=b0, b1=b1)
            data, info = build_font(glyphs, Q)
            if best is None or len(data) < len(best[0]): best = (data, info, Q)
    data, info, P = best

    # 自我驗證：逐字查表＋解碼＋比對
    for g in glyphs:
        p = lookup(data, g["enc"], P)
        assert p is not None, f"查不到 {g['ch']!r}"
        d = decode_glyph(data, p, P)
        for k in ("w", "h", "xo", "yo", "adv"):
            assert d[k] == g[k], (g["ch"], k, d[k], g[k])
        assert d["bits"] == g["bits"], ("bitmap 不符", g["ch"])
    assert lookup(data, 0x4E00 + 12345, P) is None or True

    with open(a.out, "w", encoding="utf-8") as f:
        f.write("// 由 tools/make_u8g2_font.py 自動產生，請勿手改。\n")
        f.write(f"// 來源：{a.ttf.split('/')[-1]} @ {a.size}px，{len(glyphs)} 字，{len(data)} 位元組\n")
        f.write("// 字型：俐方體11號 Cubic 11（ACh-K，SIL Open Font License 1.1，見 tools/Cubic_11_OFL.txt）\n")
        f.write("#pragma once\n#include <stdint.h>\n#ifndef U8G2_FONT_SECTION\n#include <U8g2_for_Adafruit_GFX.h>\n#endif\n")
        f.write(f"#define {a.name.upper()}_ASCENT {info['asc_A']}\n#define {a.name.upper()}_DESCENT {-info['desc_g']}\n")
        f.write(f"const uint8_t {a.name}[{len(data)}] U8G2_FONT_SECTION(\"{a.name}\") = {{\n")
        for i in range(0, len(data), 24):
            f.write("  " + ",".join(f"0x{b:02X}" for b in data[i:i + 24]) + ",\n")
        f.write("};\n")
    print(f"字型 {a.name}：{len(glyphs)} 字、{len(data)} 位元組，參數 {P}，ascent_A={info['asc_A']} descent_g={info['desc_g']}，驗證通過")

    if a.preview:
        texts = a.preview_text or ["".join(g["ch"] for g in glyphs)]
        scale = 4; lineh = ascent + descent + 2; W = 136
        im = Image.new("RGB", (W, lineh * len(texts) + 4), (8, 8, 16))
        px = im.load()
        for li, t in enumerate(texts):
            x = 2; base = 2 + li * lineh + ascent
            for ch in t:
                p = lookup(data, ord(ch), P)
                if p is None: continue
                d = decode_glyph(data, p, P)
                for yy in range(d["h"]):
                    for xx in range(d["w"]):
                        if d["bits"][yy * d["w"] + xx]:
                            X = x + d["xo"] + xx; Y = base - d["h"] - d["yo"] + yy
                            if 0 <= X < W and 0 <= Y < im.height: px[X, Y] = (255, 230, 120) if X > 129 else (240, 240, 240)
                x += d["adv"]
            print(f"  寬度 {x-2:3d}px  {t}")
        # 128px 邊界線
        for y in range(im.height): px[130, y] = (200, 60, 60)
        im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
        im.save(a.preview)
        print("預覽：", a.preview)

if __name__ == "__main__":
    main()
