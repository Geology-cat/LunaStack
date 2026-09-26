#!/usr/bin/env python3
"""RAW読み込みの試験画像（小さな DNG・CR2）を作る。

    python3 tests/data/make_raw_fixtures.py

画素値は式で決めてあり、tests/src/test_raw_reader.cpp が同じ式で期待値を計算する。
ロスレスJPEGは自前の簡単な符号器（予測子1、符号長5bitの固定ハフマン表）で作る。
作ったファイルは LibRaw でも同じ値に読めることを確かめてある（開発記録 §8）。
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def value(x, y):
    """有効領域の生の値（黒 100〜130・白 4000 の範囲に収まる）。"""
    return 200 + ((x * 37 + y * 101 + (x * y) % 7) % 3700)


# ---- ロスレスJPEG -------------------------------------------------------------

class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, v, bits):
        for i in range(bits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((v >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                if self.acc == 0xFF:
                    self.out.append(0)
                self.acc = 0
                self.n = 0

    def flush(self):
        while self.n:
            self.put(1, 1)
        return bytes(self.out)


def lj92_encode(rows, width, height, comps, precision):
    """rows: height 行 × (width*comps) 標本。予測子1。"""
    bw = BitWriter()
    prev = None
    for y in range(height):
        row = rows[y]
        for x in range(width):
            for c in range(comps):
                i = x * comps + c
                if y == 0:
                    pred = (1 << (precision - 1)) if x == 0 else row[i - comps]
                elif x == 0:
                    pred = prev[i]
                else:
                    pred = row[i - comps]
                diff = (row[i] - pred) & 0xFFFF
                if diff >= 32768:
                    diff -= 65536
                if diff == 0:
                    bw.put(0, 5)
                elif diff == -32768:
                    bw.put(16, 5)
                else:
                    s = abs(diff).bit_length()
                    bw.put(s, 5)
                    bw.put(diff if diff > 0 else diff + (1 << s) - 1, s)
        prev = row
    data = bw.flush()
    counts = [0] * 16
    counts[4] = 17  # 符号長5bitに記号0〜16
    dht = bytes([0x00]) + bytes(counts) + bytes(range(17))
    sof = struct.pack('>BHHB', precision, height, width, comps)
    for c in range(comps):
        sof += bytes([c + 1, 0x11, 0])
    sos = bytes([comps]) + b''.join(bytes([c + 1, 0x00]) for c in range(comps)) + bytes([1, 0, 0])
    seg = lambda m, body: b'\xff' + bytes([m]) + struct.pack('>H', len(body) + 2) + body
    return b'\xff\xd8' + seg(0xC4, dht) + seg(0xC3, sof) + seg(0xDA, sos) + data + b'\xff\xd9'


# ---- TIFF -------------------------------------------------------------------

TYPES = {'B': (1, 1), 'A': (2, 1), 'H': (3, 2), 'L': (4, 4), 'R': (5, 8), 'U': (7, 1),
         'h': (8, 2), 'r': (10, 8)}


class Tiff:
    """IFD を並べて1つのTIFFにする。値は (タグ, 型, 値の並び)。"""

    def __init__(self, little=True, header_extra=b''):
        self.e = '<' if little else '>'
        self.little = little
        self.header_extra = header_extra
        self.blobs = []  # (名前, バイト列)

    def pack_values(self, typ, vals):
        e = self.e
        if typ == 'A':
            return vals.encode() + b'\0' if isinstance(vals, str) else vals
        if typ == 'U':
            return bytes(vals)
        out = b''
        for v in vals:
            if typ == 'B':
                out += struct.pack(e + 'B', v)
            elif typ == 'H':
                out += struct.pack(e + 'H', v)
            elif typ == 'h':
                out += struct.pack(e + 'h', v)
            elif typ == 'L':
                out += struct.pack(e + 'L', v)
            elif typ == 'R':
                out += struct.pack(e + 'LL', v[0], v[1])
            elif typ == 'r':
                out += struct.pack(e + 'll', v[0], v[1])
        return out

    def build(self, ifds, links=None):
        """ifds: 名前 → 項目の並び。値に ('@名前',) を書くとその位置（IFD/データ）になる。
        links: IFD の連鎖（先頭から順、先頭は IFD0）。"""
        e = self.e
        header = (b'II' if self.little else b'MM') + struct.pack(e + 'HL', 42, 0) + self.header_extra
        # 1周目で大きさを決め、2周目で位置を埋める。
        positions = {}
        order = list(ifds.keys())
        for pass_no in range(2):
            out = bytearray(header)
            for name, blob in self.blobs:
                positions[name] = len(out)
                out += blob
                if len(out) % 2:
                    out += b'\0'
            for name in order:
                entries = sorted(ifds[name], key=lambda t: t[0])
                positions[name] = len(out)
                ifd = bytearray(struct.pack(e + 'H', len(entries)))
                data = bytearray()
                data_base = len(out) + 2 + 12 * len(entries) + 4
                for tag, typ, vals in entries:
                    if isinstance(vals, tuple) and vals and isinstance(vals[0], str) and vals[0].startswith('@'):
                        vals = [positions.get(v[1:], 0) for v in vals]
                    raw = self.pack_values(typ, vals)
                    code, size = TYPES[typ]
                    count = len(raw) // size
                    if len(raw) <= 4:
                        field = raw + b'\0' * (4 - len(raw))
                    else:
                        field = struct.pack(e + 'L', data_base + len(data))
                        data += raw
                        if len(data) % 2:
                            data += b'\0'
                    ifd += struct.pack(e + 'HHL', tag, code, count) + field
                nxt = 0
                if links and name in links:
                    i = links.index(name)
                    if i + 1 < len(links):
                        nxt = positions.get(links[i + 1], 0)
                ifd += struct.pack(e + 'L', nxt)
                out += ifd + data
            first = positions[links[0] if links else order[0]]
            struct.pack_into(e + 'L', out, 4, first)
        return bytes(out), positions

    def add_blob(self, name, data):
        self.blobs.append((name, data))


def u16_bytes(vals, little):
    return struct.pack(('<' if little else '>') + '%dH' % len(vals), *vals)


# ---- DNG --------------------------------------------------------------------

W, H = 40, 30
# ActiveArea: 上3・左2・下29・右38（上端を奇数にして、並びの計算を確かめる）
AREA = (3, 2, 29, 38)
CROP_ORIGIN = (1, 2)   # ActiveArea の中での (x, y)
CROP_SIZE = (33, 22)
BLACK = [100, 110, 120, 130]  # 2×2 の繰り返し（ActiveArea の左上から）
WHITE = 4000


def cfa_values():
    v = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            inside = AREA[0] <= y < AREA[2] and AREA[1] <= x < AREA[3]
            v[y][x] = value(x, y) if inside else 60
    return v


def dng_common(pattern):
    return [
        (254, 'L', [0]),
        (256, 'L', [W]), (257, 'L', [H]),
        (262, 'H', [32803]),
        (271, 'A', 'LunaStack'), (272, 'A', 'Test CFA'),
        (277, 'H', [1]),
        (284, 'H', [1]),
        (33421, 'H', [2, 2]), (33422, 'B', pattern),
        (50706, 'B', [1, 4, 0, 0]), (50708, 'A', 'LunaStack Test CFA'),
        (50713, 'H', [2, 2]), (50714, 'H', BLACK), (50717, 'H', [WHITE]),
        (50719, 'L', list(CROP_ORIGIN)), (50720, 'L', list(CROP_SIZE)),
        (50829, 'L', list(AREA)),
    ]


def write_cfa16(path):
    """16bit 無圧縮・リトルエンディアン・ストリップ。並び GRBG（1,0,2,1）。"""
    v = cfa_values()
    t = Tiff(little=True)
    rows_per_strip = 8
    strips = []
    for y0 in range(0, H, rows_per_strip):
        chunk = b''.join(u16_bytes(v[y], True) for y in range(y0, min(H, y0 + rows_per_strip)))
        name = 's%d' % y0
        t.add_blob(name, chunk)
        strips.append((name, len(chunk)))
    entries = dng_common([1, 0, 2, 1]) + [
        (258, 'H', [16]), (259, 'H', [1]), (278, 'L', [rows_per_strip]),
        (273, 'L', tuple('@' + n for n, _ in strips)), (279, 'L', [n for _, n in strips]),
    ]
    data, _ = t.build({'raw': entries}, ['raw'])
    open(path, 'wb').write(data)


def write_cfa_ljpeg_subifd(path):
    """ロスレスJPEGのタイル（2成分）・RAWは SubIFD、IFD0 は小さな縮小版。並び RGGB。"""
    v = cfa_values()
    t = Tiff(little=True)
    tw, th = 16, 16
    tiles = []
    for ty in range(0, H, th):
        for tx in range(0, W, tw):
            rows = []
            for y in range(th):
                row = []
                for x in range(tw):
                    yy, xx = ty + y, tx + x
                    row.append(v[yy][xx] if yy < H and xx < W else 0)
                rows.append(row)
            # 1行 = 2成分 × (tw/2) MCU
            jpeg = lj92_encode(rows, tw // 2, th, 2, 12)
            name = 't%d_%d' % (ty, tx)
            t.add_blob(name, jpeg)
            tiles.append((name, len(jpeg)))
    t.add_blob('preview', bytes([128] * (4 * 3 * 3)))
    raw = dng_common([0, 1, 1, 2]) + [
        (258, 'H', [12]), (259, 'H', [7]),
        (322, 'L', [tw]), (323, 'L', [th]),
        (324, 'L', tuple('@' + n for n, _ in tiles)), (325, 'L', [n for _, n in tiles]),
    ]
    ifd0 = [
        (254, 'L', [1]), (256, 'L', [4]), (257, 'L', [3]), (258, 'H', [8, 8, 8]), (259, 'H', [1]),
        (262, 'H', [2]), (271, 'A', 'LunaStack'), (272, 'A', 'Test CFA'), (273, 'L', ('@preview',)),
        (277, 'H', [3]), (278, 'L', [3]), (279, 'L', [36]),
        (330, 'L', ('@raw',)),
        (34665, 'L', ('@exif',)),
        (50706, 'B', [1, 4, 0, 0]),
    ]
    exif = [(36867, 'A', '2024:01:19 02:24:06'), (37521, 'A', '25'), (36881, 'A', '+09:00')]
    data, _ = t.build({'ifd0': ifd0, 'raw': raw, 'exif': exif}, ['ifd0'])
    open(path, 'wb').write(data)


def write_cfa12_packed_be(path):
    """12bit を詰めた無圧縮・ビッグエンディアン・1ストリップ。並び BGGR。
    LinearizationTable と BlackLevelDeltaH/V も付ける。"""
    v = cfa_values()
    t = Tiff(little=False)
    out = bytearray()
    for y in range(H):
        acc = 0
        n = 0
        for x in range(W):
            acc = (acc << 12) | v[y][x]
            n += 12
            while n >= 8:
                out.append((acc >> (n - 8)) & 0xFF)
                n -= 8
        if n:
            out.append((acc << (8 - n)) & 0xFF)
    t.add_blob('strip', bytes(out))
    # 線形化: 生の値 v → 2v（0〜8191 の範囲の表）。白も2倍にする。
    table = [min(65535, 2 * i) for i in range(4096)]
    area_w = AREA[3] - AREA[1]
    area_h = AREA[2] - AREA[0]
    delta_h = [(x % 3, 1) for x in range(area_w)]        # 0,1,2 の繰り返し
    delta_v = [(-(y % 2), 1) for y in range(area_h)]     # 0,-1 の繰り返し
    entries = [e for e in dng_common([2, 1, 1, 0]) if e[0] not in (50714, 50717)] + [
        (258, 'H', [12]), (259, 'H', [1]), (278, 'L', [H]),
        (273, 'L', ('@strip',)), (279, 'L', [len(out)]),
        (50712, 'H', table),
        (50714, 'H', [2 * b for b in BLACK]), (50717, 'L', [2 * WHITE]),
        (50715, 'r', delta_h), (50716, 'r', delta_v),
    ]
    data, _ = t.build({'raw': entries}, ['raw'])
    open(path, 'wb').write(data)


def float_to_half(f):
    return struct.unpack('<H', struct.pack('<e', f))[0]


def write_linear_f16_deflate(path):
    """LinearRaw（3標本）16bit浮動小数点・Deflate・浮動小数点予測子・タイル。"""
    t = Tiff(little=True)
    tw, th = 16, 8
    tiles = []
    for ty in range(0, H, th):
        for tx in range(0, W, tw):
            raw = bytearray()
            for y in range(th):
                row = []
                for x in range(tw):
                    for c in range(3):
                        yy, xx = ty + y, tx + x
                        val = (value(xx, yy) / 4000.0) * (0.5 + 0.25 * c) if yy < H and xx < W else 0.0
                        row.append(float_to_half(val))
                # 浮動小数点予測子: 標本を上位バイトから順に分けて並べ、バイトの差分を取る。
                count = len(row)
                planes = bytearray(2 * count)
                for i, hv in enumerate(row):
                    planes[i] = hv >> 8
                    planes[count + i] = hv & 0xFF
                for i in range(len(planes) - 1, 2, -1):
                    planes[i] = (planes[i] - planes[i - 3]) & 0xFF
                raw += planes
            comp = zlib.compress(bytes(raw), 9)
            name = 't%d_%d' % (ty, tx)
            t.add_blob(name, comp)
            tiles.append((name, len(comp)))
    entries = [
        (254, 'L', [0]), (256, 'L', [W]), (257, 'L', [H]), (258, 'H', [16, 16, 16]), (259, 'H', [8]),
        (262, 'H', [34892]), (271, 'A', 'LunaStack'), (272, 'A', 'Test Linear'), (277, 'H', [3]),
        (284, 'H', [1]), (317, 'H', [3]), (339, 'H', [3, 3, 3]),
        (322, 'L', [tw]), (323, 'L', [th]),
        (324, 'L', tuple('@' + n for n, _ in tiles)), (325, 'L', [n for _, n in tiles]),
        (50706, 'B', [1, 4, 0, 0]), (50717, 'H', [1, 1, 1]),
    ]
    data, _ = t.build({'raw': entries}, ['raw'])
    open(path, 'wb').write(data)


# ---- CR2 --------------------------------------------------------------------

CR2_W, CR2_H = 44, 26
# SensorInfo の境界（右・下は含む）。左は遮光部を16画素以上取る。
CR2_LEFT, CR2_TOP, CR2_RIGHT, CR2_BOTTOM = 20, 3, 41, 24
CR2_BLACK = [512, 516, 520, 524]  # 生の座標の位相 (y&1)*2+(x&1) ごと
SLICES = [2, 16, 12]  # 幅16の帯2本と幅12の帯1本（計44）


def cr2_values():
    v = [[0] * CR2_W for _ in range(CR2_H)]
    for y in range(CR2_H):
        for x in range(CR2_W):
            if CR2_LEFT <= x <= CR2_RIGHT and CR2_TOP <= y <= CR2_BOTTOM:
                v[y][x] = CR2_BLACK[(y & 1) * 2 + (x & 1)] + value(x, y)
            else:
                v[y][x] = CR2_BLACK[(y & 1) * 2 + (x & 1)]
    return v


def write_cr2(path):
    v = cr2_values()
    # 帯ごとに上から行を詰めた標本列を作り、JPEG の行（2成分 × 11 MCU = 22標本）に切る。
    seq = []
    x0 = 0
    for k in range(SLICES[0] + 1):
        width = SLICES[1] if k < SLICES[0] else SLICES[2]
        for y in range(CR2_H):
            seq.extend(v[y][x0:x0 + width])
        x0 += width
    jw = 22
    rows = [seq[i:i + jw] for i in range(0, len(seq), jw)]
    jpeg = lj92_encode(rows, jw // 2, len(rows), 2, 14)

    t = Tiff(little=True, header_extra=b'CR\x02\x00' + b'\0\0\0\0')
    t.add_blob('raw', jpeg)
    sensor = [34, CR2_W, CR2_H, 1, 1, CR2_LEFT, CR2_TOP, CR2_RIGHT, CR2_BOTTOM,
              0, 0, 0, 0, 0, 0, 0, 0]
    makernote = [(0x00E0, 'H', sensor), (0x0035, 'L', [16, 540, 6, 0])]
    exif = [(36867, 'A', '2025:07:13 22:27:54'), (37521, 'A', '14'), (37500, 'U', b'')]
    ifd0 = [(271, 'A', 'Canon'), (272, 'A', 'Canon Test Body'), (34665, 'L', ('@exif',))]
    ifd1 = [(256, 'L', [1])]
    ifd2 = [(256, 'L', [1])]
    ifd3 = [(259, 'H', [6]), (273, 'L', ('@raw',)), (279, 'L', [len(jpeg)]),
            (0xC5E0, 'L', [1]), (0xC640, 'H', SLICES)]
    ifds = {'ifd0': ifd0, 'exif': exif, 'mn': makernote, 'ifd1': ifd1, 'ifd2': ifd2, 'ifd3': ifd3}
    data, pos = t.build(ifds, ['ifd0', 'ifd1', 'ifd2', 'ifd3'])
    # メーカーノートの値の位置を、メーカーノートのIFDに向け直す（Canon は素のIFD）。
    buf = bytearray(data)
    exif_pos = pos['exif']
    count = struct.unpack_from('<H', buf, exif_pos)[0]
    for i in range(count):
        e = exif_pos + 2 + 12 * i
        tag = struct.unpack_from('<H', buf, e)[0]
        if tag == 37500:
            size = 2 + 12 * len(makernote) + 4
            struct.pack_into('<LL', buf, e + 4, size, pos['mn'])
    struct.pack_into('<L', buf, 12, pos['ifd3'])  # CR2 ヘッダの「RAWのIFD」
    open(path, 'wb').write(bytes(buf))


if __name__ == '__main__':
    write_cfa16(os.path.join(HERE, 'raw_cfa16_grbg.dng'))
    write_cfa_ljpeg_subifd(os.path.join(HERE, 'raw_cfa_ljpeg_rggb.dng'))
    write_cfa12_packed_be(os.path.join(HERE, 'raw_cfa12_packed_bggr.dng'))
    write_linear_f16_deflate(os.path.join(HERE, 'raw_linear_f16_deflate.dng'))
    write_cr2(os.path.join(HERE, 'raw_sliced.cr2'))
    print('ok')
