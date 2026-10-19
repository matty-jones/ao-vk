#!/usr/bin/env python3
"""Decode a playfield's ground from the client's rdb.db (docs/ground-format.md) and render it: hill-shaded heights,
the tiles in their textures' mean colours, and the six baked lightmaps. Research tool for the native ground renderer.

Usage: tools/ground-data.py <playfield id> <out dir>        e.g. 790 build/ground   -> heights_790.png, tiles_790.png,
                                                                                        lightmaps_790.png
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
    n, _ = struct.unpack_from("<HH", d, 0x18)
    tex = struct.unpack_from(f"<{n}H", d, 0x1C)
    # The archive's fields: u8 key (index into its string table), u32 type, u32 element size, u32 bytes, the bytes.
    # Its first field is map_width (key 4, an int32); the string table and object header before it are skipped.
    r = d.index(bytes.fromhex("04 03 00 00 00 04 00 00 00 04 00 00 00"), 0x1C + 2 * n)
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
            # Deltas: summed down each column, then along each row (Patch_t::DecompressHeightMap); bytes wrap at 256
            # and are the height x 256, 8-byte small data marks 16-bit heights.
            if len(small[i]) == 8:
                a = np.cumsum(np.cumsum(np.frombuffer(raw, "<i2").reshape(P + 1, P + 1).astype(np.int64), 0), 1)
                hp = (a & 0xFFFF).astype(np.float32) / 256.0
            else:
                a = np.cumsum(np.cumsum(np.frombuffer(raw, np.uint8).reshape(P + 1, P + 1).astype(np.int64), 0), 1)
                hp = (a & 0xFF).astype(np.float32)
            heights[pz * P:pz * P + P + 1, px * P:px * P + P + 1] = hp * hscale
            tiles[pz * P:pz * P + P, px * P:px * P + P] = np.frombuffer(zlib.decompress(tm[i]), "<u2").reshape(P, P)
    return dict(size=(w, h), cell=cell, hscale=hscale, P=P, tex=tex, heights=heights, tiles=tiles)


def lightmaps(db, pf):
    """The six baked lightmaps (rdb_1000007): u32 offsets, then PNGs of 4-bit texels, two to a byte (low first)."""
    row = db.execute(f"select data from rdb_{T_LIGHTMAPS} where id=?", (pf,)).fetchone()
    out, d = [], row[0] if row else b""
    i = d.find(b"\x89PNG")
    while i != -1:
        a = np.asarray(Image.open(io.BytesIO(d[i:])))
        m = np.empty((a.shape[0], a.shape[1] * 2), np.uint8)
        m[:, 0::2], m[:, 1::2] = a & 15, a >> 4
        out.append(m)
        i = d.find(b"\x89PNG", i + 8)
    return out


def main():
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
