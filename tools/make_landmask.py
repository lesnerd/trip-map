"""Rasterise Natural Earth 110m land into a bit mask the map kernel can sample.

Row 0 is north. Each bit is one cell, most-significant bit first.
Regenerate with:  python tools/make_landmask.py
"""
import json
import math
import pathlib
import urllib.request

W, H = 720, 360
URLS = [
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_110m_land.geojson",
    "https://cdn.jsdelivr.net/gh/nvkelso/natural-earth-vector@master/geojson/ne_110m_land.geojson",
]


def download():
    last = None
    for url in URLS:
        try:
            with urllib.request.urlopen(url, timeout=60) as resp:
                return json.load(resp)
        except Exception as exc:  # noqa: BLE001 - try the next mirror
            last = exc
            print(f"fetch failed: {url}: {exc}")
    raise SystemExit(f"could not download land polygons: {last}")


def add_segment(edges, x0, y0, x1, y1):
    dx = x1 - x0
    if abs(dx) <= 180:
        edges.append((x0, y0, x1, y1))
        return
    # The short way crosses the date line. Unwrap, then split at ±180.
    if dx > 180:
        x1 -= 360
    else:
        x1 += 360
    if x1 < x0:
        x0, x1 = x1, x0
        y0, y1 = y1, y0
    if x0 < 180 < x1 and x1 != x0:
        t = (180 - x0) / (x1 - x0)
        ym = y0 + t * (y1 - y0)
        edges.append((x0, y0, 180.0, ym))
        edges.append((-180.0, ym, x1 - 360.0, y1))
    else:
        edges.append((x0, y0, x1, y1))


def rings_of(geometry):
    gtype = geometry.get("type")
    coords = geometry.get("coordinates") or []
    if gtype == "Polygon":
        return coords
    if gtype == "MultiPolygon":
        out = []
        for poly in coords:
            out.extend(poly)
        return out
    return []


def collect_edges(geo):
    edges = []
    for feature in geo.get("features", []):
        geometry = feature.get("geometry") or {}
        for ring in rings_of(geometry):
            if len(ring) < 2:
                continue
            for a, b in zip(ring, ring[1:]):
                add_segment(edges, float(a[0]), float(a[1]), float(b[0]), float(b[1]))
    return edges


def raster(edges):
    bits = bytearray((W * H) // 8)
    for y in range(H):
        lat = 90.0 - (y + 0.5) * (180.0 / H)
        xs = []
        for x0, y0, x1, y1 in edges:
            # Count the lower vertex only, so a shared vertex is not a double crossing.
            if (y0 <= lat < y1) or (y1 <= lat < y0):
                xs.append(x0 + (lat - y0) * (x1 - x0) / (y1 - y0))
        xs.sort()
        row = y * W
        for k in range(0, len(xs) - 1, 2):
            a, b = xs[k], xs[k + 1]
            x_start = max(0, math.ceil((a + 180.0) / 360.0 * W - 0.5))
            x_end = min(W, math.floor((b + 180.0) / 360.0 * W - 0.5) + 1)
            for x in range(int(x_start), int(x_end)):
                i = row + x
                bits[i >> 3] |= 1 << (7 - (i & 7))
    return bits


def main():
    geo = download()
    edges = collect_edges(geo)
    bits = raster(edges)
    land = sum(bin(v).count("1") for v in bits)
    fraction = land / (W * H)
    print(f"edges {len(edges)}  land pixels {land}  fraction {fraction:.3f}")
    if not 0.22 <= fraction <= 0.40:
        raise SystemExit(f"land fraction {fraction:.3f} is outside the expected range")

    root = pathlib.Path(__file__).resolve().parents[1]
    out = root / "src" / "landmask.cpp"
    lines = [
        '#include "landmask.h"',
        "",
        "// 720 x 360 equirectangular land mask, row 0 = north, MSB first.",
        "// Built by tools/make_landmask.py from Natural Earth 110m land.",
        "const unsigned char kLandMask[kLandMaskBytes] = {",
    ]
    row = []
    for i, b in enumerate(bits):
        row.append(f"0x{b:02x}")
        if len(row) == 16 or i + 1 == len(bits):
            lines.append("    " + ", ".join(row) + ",")
            row = []
    lines.append("};")
    lines.append("")
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
