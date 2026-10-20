#!/usr/bin/env python3
"""Decode a playfield's ground from the client's rdb.db (docs/ground-format.md) and render it: hill-shaded heights,
the tiles in their textures' mean colours, and the six baked lightmaps. Research tool for the native ground renderer.

Usage: tools/ground-data.py <playfield id> <out dir>        e.g. 790 build/ground   -> heights_790.png, tiles_790.png,
                                                                                        lightmaps_790.png
       tools/ground-data.py <playfield id> --export <file> [--map M] [--shade S]
           the ground as rvk_demo --native-ground-file reads it: "AOGND2\0\0", u32 cells across, down, patch,
           f32 cell x, z; f32 heights[(across + 1) x (down + 1)]; u16 codes[across x down]; u32 tiles; per tile u32
           bytes + its JPEG (rdb_1010006); u32 lightmap w, h, f32 shade, its packed bytes (map M of the six, 3);
           u32 n, u16 each tile's surface type[n] (tile_type_data; AOGND1 ends before it)
       tools/ground-data.py <playfield id> --types
           each surface type's base tile and how the pebbles (rvk ground_pebbles.cpp) take it: its look and verdict
"""
import io
import pathlib
import sqlite3
import struct
import sys
import zlib

import numpy as np
from PIL import Image

DB = pathlib.Path.home() / ".wine-prk/drive_c/linux/client/cd_image/rdb.db"
T_GROUND, T_LIGHTMAPS, T_TILE_LOW = 1000009, 1000007, 1010022


def connect():
    return sqlite3.connect(f"file:{DB}?mode=ro", uri=True)


def load(db, pf):
    """The ground (rdb_1000009): the CHGA header, the tile textures, then the AnarchyGroundDataDB_t archive."""
    d = db.execute(f"select data from rdb_{T_GROUND} where id=?", (pf,)).fetchone()[0]
    magic, _, _, w, h, cell, hscale = struct.unpack_from("<4sIIHHff", d, 0)
    assert magic == b"CHGA", magic
    n, = struct.unpack_from("<H", d, 0x18)
    tex = struct.unpack_from(f"<{n}H", d, 0x1A)       # (a tile code's low 14 bits index this)
    # The archive's fields: u8 key (index into its string table), u32 type, u32 element size, u32 bytes, the bytes.
    # Its first field is map_width (key 4, an int32); the string table and object header before it are skipped.
    r = d.index(bytes.fromhex("04 03 00 00 00 04 00 00 00 04 00 00 00"), 0x1A + 2 * n)
    fields = {}
    while r < len(d) - 13:
        key, typ, _, size = struct.unpack_from("<BIII", d, r)
        if typ > 64 or size > len(d):
            break                                  # (the per-patch texture objects follow)
        fields[key] = (r + 13, size)
        r += 13 + size

    def int32(key):
        return struct.unpack_from("<i", d, fields[key][0])[0]

    def array(key):                                # type 9: u32 size + bytes per element
        s, size = fields[key]
        end, out = s + size, []
        while s < end:
            m, = struct.unpack_from("<I", d, s)
            out.append(d[s + 4:s + 4 + m])
            s += 4 + m
        return out

    mw, mh = int32(4), int32(5)
    tz = lambda v: (v & -v).bit_length() - 1
    P = 1 << min(tz(mw - 1), tz(mh - 1), 6)        # cells a patch side (AnarchyGroundData_t +0x40)
    pw, ph = (mw - 1) // P, (mh - 1) // P
    hm, small, tm = array(8), array(9), array(10)
    heights = np.zeros((mh, mw), np.float32)
    tiles = np.zeros((mh - 1, mw - 1), np.uint16)
    for pz in range(ph):
        for px in range(pw):
            i = pz * pw + px
            raw = zlib.decompress(hm[i])
            # Deltas: summed down each column, then along each row (Patch_t::DecompressHeightMap). Bytes wrap at 256
            # and are the height x 256, the height unit then the header's step / 256 (AnarchyGround_t::SetScale);
            # 8-byte small data marks 16-bit heights, in the header's step itself.
            if len(small[i]) == 8:
                a = np.cumsum(np.cumsum(np.frombuffer(raw, "<u2").reshape(P + 1, P + 1).astype(np.int64), 0), 1)
                hp = (a & 0xFFFF).astype(np.float32)
            else:
                a = np.cumsum(np.cumsum(np.frombuffer(raw, np.uint8).reshape(P + 1, P + 1).astype(np.int64), 0), 1)
                hp = (a & 0xFF).astype(np.float32)
            heights[pz * P:pz * P + P + 1, px * P:px * P + P + 1] = hp * hscale
            tiles[pz * P:pz * P + P, px * P:px * P + P] = np.frombuffer(zlib.decompress(tm[i]), "<u2").reshape(P, P)
    types = np.frombuffer(array(21)[0], "<u2") if 21 in fields else np.zeros(0, np.uint16)   # tile_type_data
    return dict(size=(w, h), cell=cell, hscale=hscale, P=P, tex=tex, heights=heights, tiles=tiles, types=types)


def lightmaps(db, pf):
    """The six baked lightmaps (rdb_1000007): u32 offsets, then PNGs of 4-bit texels, two to a byte (the high nibble
    first - AnarchyGround_t::SetShadowMap)."""
    row = db.execute(f"select data from rdb_{T_LIGHTMAPS} where id=?", (pf,)).fetchone()
    out, d = [], row[0] if row else b""
    i = d.find(b"\x89PNG")
    while i != -1:
        a = np.asarray(Image.open(io.BytesIO(d[i:])))
        m = np.empty((a.shape[0], a.shape[1] * 2), np.uint8)
        m[:, 0::2], m[:, 1::2] = a >> 4, a & 15
        out.append(m)
        i = d.find(b"\x89PNG", i + 8)
    return out


def export(pf, path, map_index, shade):
    db = connect()
    g = load(db, pf)
    H, T = g["heights"], g["tiles"]
    rows, cols = T.shape
    out = bytearray(b"AOGND2\0\0")
    out += struct.pack("<IIIff", cols, rows, g["P"], g["cell"], g["cell"])
    out += H.astype("<f4").tobytes()
    out += T.astype("<u2").tobytes()
    out += struct.pack("<I", len(g["tex"]))
    for rid in g["tex"]:
        row = db.execute("select data from rdb_1010006 where id=?", (rid,)).fetchone()
        jpeg = row[0][24:] if row else b""
        out += struct.pack("<I", len(jpeg)) + jpeg
    maps = lightmaps(db, pf)
    m = maps[map_index] if maps else np.zeros((rows, cols), np.uint8)
    packed = ((m[:, 0::2] << 4) | m[:, 1::2]).astype(np.uint8)
    out += struct.pack("<IIf", m.shape[1], m.shape[0], shade) + packed.tobytes()
    out += struct.pack("<I", len(g["types"])) + g["types"].astype("<u2").tobytes()
    pathlib.Path(path).write_bytes(out)
    print(f"-> {path}: {cols} x {rows} cells, {len(g['tex'])} tiles, lightmap {map_index} ({len(out) / 1e6:.1f} MB)")


def look(rgb):
    """A tile's look as rvk measures it at upload (resources.cpp PixelsLook): line, edge (its rows' and columns' mean
    brightness on a 64 x 64 grid against its spread), grain (the brightness step two texels apart, 32 x 32 grid)."""
    L = rgb.astype(np.float32) @ np.array([0.3, 0.59, 0.11], np.float32)
    n = L.shape[0]
    s = L[2 * n // 256::4 * n // 256, 2 * n // 256::4 * n // 256][:64, :64]
    spread = max(s.std(), 1.0)
    rows, cols = s.mean(1), s.mean(0)
    line = max(rows.std(), cols.std()) / spread
    edge = max(np.abs(np.diff(rows)).max(), np.abs(np.diff(cols)).max()) / spread
    two = max(n // 128, 1)
    a = L[2 * n // 256::8 * n // 256, 2 * n // 256::8 * n // 256][:32, :32]
    b = L[2 * n // 256::8 * n // 256, 2 * n // 256 + two::8 * n // 256][:32, :32]
    c = L[2 * n // 256 + two::8 * n // 256, 2 * n // 256::8 * n // 256][:32, :32]
    grain = 0.5 * (np.abs(a - b).mean() + np.abs(a - c).mean())
    return line, edge, grain


def verdict(mean, line, edge, grain):
    """How many pebbles a ground gets from its base tile's look (rvk ground_pebbles.cpp MakePebbleTable): density 0..1."""
    r, g, b = mean
    hi, lo = max(mean), min(mean)
    lum, sat = 0.3 * r + 0.59 * g + 0.11 * b, (hi - lo) / max(hi, 1)
    if g > r * 1.08 and g > b * 1.08:
        return "grass", 0.0
    if line > 0.3 or edge > 1.2:
        return "slabs", 0.0
    if sat < 0.12 and (grain < 15 or (sat < 0.08 and lum > 150)):
        return "smooth grey", 0.0
    if b > r * 1.05 and sat < 0.25:            # (paved: a cool grey - natural ground is warm)
        return "paved", 0.0
    if grain > 50:
        return "straw", 0.3
    r, g, b = mean
    if r >= g >= b and grain < 32 and sat > 0.4 and lum > 128:
        return "sand", 0.05
    d = min(max(0.2 + (grain - 8) / 30, 0.2), 1.0) * (1.0 if sat < 0.2 else 0.7)
    return ("gravel" if sat < 0.2 and grain > 25 else "dirt"), d


def surface_types(pf):
    db = connect()
    g = load(db, pf)
    ty, T = g["types"], g["tiles"] & 0x3FFF
    w, h = g["size"]
    used = np.bincount(T[:h, :w].ravel(), minlength=len(g["tex"]))
    print(f"playfield {pf}: {len(g['tex'])} tiles, {int(ty.max()) + 1 if len(ty) else 0} surface types")
    for t in range(int(ty.max()) + 1 if len(ty) else 0):
        of = np.where(ty == t)[0]
        if not len(of):
            continue
        base = t if t < len(ty) and ty[t] == t else int(of[np.argmax(used[of])])
        row = db.execute("select data from rdb_1010006 where id=?", (g["tex"][base],)).fetchone()
        rgb = np.asarray(Image.open(io.BytesIO(row[0][24:])).convert("RGB"))
        thumb = rgb[rgb.shape[0] // 32::rgb.shape[0] // 16, rgb.shape[1] // 32::rgb.shape[1] // 16].reshape(-1, 3)
        mean = thumb.mean(0)
        line, edge, grain = look(rgb)
        what, d = verdict(mean, line, edge, grain)
        print(f"  type {t:2d}: tile {base:3d}, {len(of):3d} tiles, {int(used[of].sum()):7d} cells, colour "
              f"{mean.astype(int)}, line {line:.2f} edge {edge:.2f} grain {grain:5.1f} -> {what} {d:.2f}")


def main():
    if len(sys.argv) > 2 and sys.argv[2] == "--types":
        surface_types(int(sys.argv[1]))
        return
    if len(sys.argv) > 2 and sys.argv[2] == "--export":
        args = sys.argv[3:]
        opt = lambda k, d: type(d)(args[args.index(k) + 1]) if k in args else d
        export(int(sys.argv[1]), args[0], opt("--map", 3), opt("--shade", 0.45))
        return
    pf, out = int(sys.argv[1]), pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    db = connect()
    g = load(db, pf)
    H, T = g["heights"], g["tiles"]
    print(f"playfield {pf}: {g['size'][0]} x {g['size'][1]} cells of {g['cell']} units (grid {H.shape[1]} x "
          f"{H.shape[0]}, patches of {g['P']}), heights {H.min():.1f}..{H.max():.1f}, {len(g['tex'])} tiles")
    shade = np.clip(0.6 + (np.gradient(H, axis=1) - np.gradient(H, axis=0)) * 0.6 / g["hscale"] * 0.2, 0, 1)
    Image.fromarray((shade * 255).astype(np.uint8)).save(out / f"heights_{pf}.png")
    rgb = np.zeros(T.shape + (3,))
    types = T & 0x3FFF
    for t, rid in enumerate(g["tex"]):
        row = db.execute(f"select data from rdb_{T_TILE_LOW} where id=?", (rid,)).fetchone()
        if row:
            rgb[types == t] = np.asarray(Image.open(io.BytesIO(row[0][24:])).convert("RGB")).reshape(-1, 3).mean(0)
    rgb *= 0.55 + 0.45 * shade[:-1, :-1, None]
    Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).save(out / f"tiles_{pf}.png")
    maps = lightmaps(db, pf)
    if maps:
        sheet = np.concatenate([np.pad(m * 17, ((0, 0), (0, 8))) for m in maps], axis=1).astype(np.uint8)
        Image.fromarray(sheet).save(out / f"lightmaps_{pf}.png")
    print(f"-> {out}/heights_{pf}.png, tiles_{pf}.png" + (f", lightmaps_{pf}.png ({len(maps)})" if maps else ""))


if __name__ == "__main__":
    main()
