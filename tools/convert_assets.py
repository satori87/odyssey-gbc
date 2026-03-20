#!/usr/bin/env python3
"""Convert bg.png tileset to GBDK-compatible C data and generate lookup tables."""

import math
import os
from PIL import Image

ASSET_PATH = os.path.join(os.path.dirname(__file__), "..", "assets", "bg.png")
OUT_PATH = os.path.join(os.path.dirname(__file__), "..", "include")

# 4-color palette sorted by luminance (lightest=0, darkest=3)
# White (255,255,255) merged into cream
TARGET_PALETTE = [
    (255, 246, 211),  # 0: cream (lightest)
    (218, 122, 52),   # 1: orange/brown
    (142, 47, 21),    # 2: dark brown
    (42, 5, 3),       # 3: near-black (darkest)
]

SHADE_LEVELS = [1.0, 0.70, 0.45, 0.25]  # 4 palette brightness levels


def color_dist_sq(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b))


def nearest_palette_index(rgb):
    best, best_d = 0, float("inf")
    for i in range(4):
        d = color_dist_sq(rgb, TARGET_PALETTE[i])
        if d < best_d:
            best_d, best = d, i
    return best


def rgb_to_gbc(r, g, b):
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def shade_color(r, g, b, factor):
    return (int(r * factor), int(g * factor), int(b * factor))


def extract_tile_2bpp(img, tx, ty, pal_map):
    data = []
    for py in range(8):
        low, high = 0, 0
        for px in range(8):
            rgb = img.getpixel((tx * 8 + px, ty * 8 + py))
            idx = pal_map.get(rgb, nearest_palette_index(rgb))
            if idx & 1:
                low |= 0x80 >> px
            if idx & 2:
                high |= 0x80 >> px
        data.append(low)
        data.append(high)
    return data


def generate_trig_tables():
    sin_t, cos_t = [], []
    for i in range(256):
        angle = i * 2.0 * math.pi / 256.0
        s = max(-128, min(127, round(math.sin(angle) * 64)))
        c = max(-128, min(127, round(math.cos(angle) * 64)))
        sin_t.append(s)
        cos_t.append(c)
    return sin_t, cos_t


def generate_height_table(max_dist_qt):
    """height_table[dist_qt] = wall height in tile rows (0-18).
    dist_qt is perpendicular distance in quarter-tile units."""
    WALL_SCALE = 72  # at dist=4 qt (1.0 tile), height=18 (full screen)
    table = [18]  # dist=0
    for d in range(1, max_dist_qt + 1):
        h = WALL_SCALE // d
        table.append(min(h, 18))
    return table


def main():
    os.makedirs(OUT_PATH, exist_ok=True)

    img = Image.open(ASSET_PATH).convert("RGB")
    w, h = img.size
    tiles_x, tiles_y = w // 8, h // 8
    total_tiles = tiles_x * tiles_y

    print(f"Image: {w}x{h}, {tiles_x}x{tiles_y} tiles = {total_tiles}")

    # Build palette map
    pal_map = {c: i for i, c in enumerate(TARGET_PALETTE)}
    pal_map[(255, 255, 255)] = 0  # merge white into cream

    # Generate 4 shaded GBC palettes
    all_gbc_palettes = []
    for shade in SHADE_LEVELS:
        for r, g, b in TARGET_PALETTE:
            sr, sg, sb = shade_color(r, g, b, shade)
            all_gbc_palettes.append(rgb_to_gbc(sr, sg, sb))

    # Extract tiles (up to 255, reserve slot 255 for ceiling)
    max_tiles = min(total_tiles, 255)
    all_tile_data = []
    for t in range(max_tiles):
        tx, ty = t % tiles_x, t // tiles_x
        all_tile_data.extend(extract_tile_2bpp(img, tx, ty, pal_map))

    # Tile 255: solid dark ceiling (all pixels = color 3)
    all_tile_data.extend([0xFF, 0xFF] * 8)
    total_loaded = max_tiles + 1

    # --- Write bg_tiles.h ---
    with open(os.path.join(OUT_PATH, "bg_tiles.h"), "w") as f:
        f.write("// Auto-generated from bg.png\n")
        f.write("#ifndef BG_TILES_H\n#define BG_TILES_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define BG_TILE_COUNT   {total_loaded}\n")
        f.write(f"#define NUM_BG_PALETTES 4\n\n")

        # 16x16 metatile sub-tile indices (row-major within the 20-wide tileset)
        # Wall metatile at pixel (0,0): tiles 0,1,20,21
        # Floor metatile at pixel (16,0): tiles 2,3,22,23
        f.write(f"// 16x16 metatile sub-tile indices [row][col]\n")
        f.write(f"#define WALL_TL  0\n")
        f.write(f"#define WALL_TR  1\n")
        f.write(f"#define WALL_BL  {tiles_x}\n")
        f.write(f"#define WALL_BR  {tiles_x + 1}\n")
        f.write(f"#define FLOOR_TL 2\n")
        f.write(f"#define FLOOR_TR 3\n")
        f.write(f"#define FLOOR_BL {tiles_x + 2}\n")
        f.write(f"#define FLOOR_BR {tiles_x + 3}\n")
        f.write(f"#define TILE_CEILING 255\n\n")

        # Palettes (4 palettes x 4 colors = 16 uint16_t)
        f.write("const uint16_t bg_palettes[16] = {\n")
        for p in range(4):
            vals = all_gbc_palettes[p * 4 : p * 4 + 4]
            label = f"{int(SHADE_LEVELS[p]*100)}%"
            f.write(f"    " + ", ".join(f"0x{v:04X}" for v in vals))
            f.write(f",  // palette {p} ({label})\n")
        f.write("};\n\n")

        # Tile data
        f.write(f"const uint8_t bg_tile_data[{len(all_tile_data)}] = {{\n")
        for i in range(0, len(all_tile_data), 16):
            chunk = all_tile_data[i : i + 16]
            f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk))
            if i + 16 < len(all_tile_data):
                f.write(",")
            f.write(f"  // tile {i // 16}\n")
        f.write("};\n\n")
        f.write("#endif\n")

    print(f"Wrote bg_tiles.h ({total_loaded} tiles, 4 palettes)")

    # --- Write lut.h ---
    sin_t, cos_t = generate_trig_tables()
    MAX_QT = 80
    height_t = generate_height_table(MAX_QT)

    with open(os.path.join(OUT_PATH, "lut.h"), "w") as f:
        f.write("// Auto-generated lookup tables\n")
        f.write("#ifndef LUT_H\n#define LUT_H\n\n")
        f.write("#include <stdint.h>\n\n")

        f.write("#define TRIG_SCALE 64\n")
        f.write(f"#define MAX_DIST_QT {MAX_QT}\n\n")

        for name, table in [("sin_table", sin_t), ("cos_table", cos_t)]:
            f.write(f"const int8_t {name}[256] = {{\n")
            for i in range(0, 256, 16):
                vals = table[i : i + 16]
                f.write("    " + ", ".join(f"{v:4d}" for v in vals))
                if i + 16 < 256:
                    f.write(",")
                f.write("\n")
            f.write("};\n\n")

        f.write(f"const uint8_t height_table[{MAX_QT + 1}] = {{\n")
        for i in range(0, len(height_t), 16):
            vals = height_t[i : i + 16]
            f.write("    " + ", ".join(f"{v:2d}" for v in vals))
            if i + 16 < len(height_t):
                f.write(",")
            f.write("\n")
        f.write("};\n\n")

        f.write("#endif\n")

    print(f"Wrote lut.h")


if __name__ == "__main__":
    main()
