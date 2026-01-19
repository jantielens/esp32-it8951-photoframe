#!/usr/bin/env python3
"""Batch-convert JPGs to packed 4bpp .g4 for IT8951 (1872x1404)."""

import argparse
import os
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageEnhance, ImageFilter, ImageOps

DEFAULT_WIDTH = 1872
DEFAULT_HEIGHT = 1404


def iter_images(paths: Iterable[Path]) -> Iterable[Path]:
    for p in paths:
        if p.is_dir():
            for ext in ("*.jpg", "*.jpeg", "*.JPG", "*.JPEG"):
                for f in p.rglob(ext):
                    yield f
        elif p.is_file():
            yield p


def fit_with_white_bg(img: Image.Image, width: int, height: int) -> Image.Image:
    img = img.convert("RGB")
    src_w, src_h = img.size
    scale = min(width / src_w, height / src_h)
    new_w = max(1, int(src_w * scale))
    new_h = max(1, int(src_h * scale))
    resized = img.resize((new_w, new_h), Image.LANCZOS)
    canvas = Image.new("RGB", (width, height), (255, 255, 255))
    left = (width - new_w) // 2
    top = (height - new_h) // 2
    canvas.paste(resized, (left, top))
    return canvas


def optimize_grayscale(img: Image.Image) -> Image.Image:
    img = ImageOps.autocontrast(img, cutoff=2)
    img = ImageEnhance.Contrast(img).enhance(1.10)
    img = img.filter(ImageFilter.UnsharpMask(radius=1.2, percent=120, threshold=3))
    return img


def add_vertical_separators(img: Image.Image, parts: int) -> Image.Image:
    if parts <= 1:
        return img
    draw = ImageDraw.Draw(img)
    w, h = img.size
    for i in range(1, parts):
        x = (w * i) // parts
        draw.line([(x, 0), (x, h)], fill=0, width=2)
    return img


def apply_bayer_dither(img: Image.Image) -> Image.Image:
    matrix = [
        [0, 8, 2, 10],
        [12, 4, 14, 6],
        [3, 11, 1, 9],
        [15, 7, 13, 5],
    ]
    step = 16
    w, h = img.size
    pixels = list(img.tobytes())
    out = bytearray(len(pixels))
    for y in range(h):
        row = y * w
        for x in range(w):
            v = pixels[row + x]
            t = matrix[y % 4][x % 4]
            v = v + int((t - 7.5))
            if v < 0:
                v = 0
            elif v > 255:
                v = 255
            q = (v + step // 2) // step
            if q > 15:
                q = 15
            out[row + x] = q * 17
    return Image.frombytes("L", (w, h), bytes(out))


def apply_floyd_steinberg_dither(img: Image.Image) -> Image.Image:
    w, h = img.size
    pixels = list(img.tobytes())
    buf = [float(p) for p in pixels]
    for y in range(h):
        for x in range(w):
            idx = y * w + x
            old = buf[idx]
            level = int(round(old / 17.0))
            if level < 0:
                level = 0
            elif level > 15:
                level = 15
            new = level * 17.0
            buf[idx] = new
            err = old - new
            if x + 1 < w:
                buf[idx + 1] += err * 7 / 16
            if y + 1 < h:
                if x > 0:
                    buf[idx + w - 1] += err * 3 / 16
                buf[idx + w] += err * 5 / 16
                if x + 1 < w:
                    buf[idx + w + 1] += err * 1 / 16
    out = bytearray(len(buf))
    for i, v in enumerate(buf):
        if v < 0:
            v = 0
        elif v > 255:
            v = 255
        out[i] = int(v + 0.5)
    return Image.frombytes("L", (w, h), bytes(out))


def pack_g4(img: Image.Image) -> bytes:
    gray = img.convert("L")
    w, h = gray.size
    if w % 2 != 0:
        raise ValueError("Width must be even for packed 4bpp")
    pixels = gray.tobytes()
    out = bytearray((w * h) // 2)
    idx = 0
    for i in range(0, len(pixels), 2):
        hi = pixels[i] >> 4
        lo = pixels[i + 1] >> 4
        out[idx] = (hi << 4) | lo
        idx += 1
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert JPGs to packed 4bpp .g4 files.")
    parser.add_argument("input", nargs="+", help="Input file(s) or folder(s)")
    parser.add_argument("--output", "-o", default=None, help="Output folder (defaults to input folder)")
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument(
        "--variant",
        choices=["base", "opt", "opt-bayer", "opt-fs", "compare"],
        default="opt-bayer",
        help="Which variant to generate (default: opt-bayer)",
    )
    args = parser.parse_args()

    out_dir = Path(args.output).expanduser().resolve() if args.output else None
    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)

    inputs = [Path(p).expanduser().resolve() for p in args.input]
    files = list(iter_images(inputs))
    if not files:
        print("No JPG files found.")
        return 1

    for src in files:
        if src.suffix.lower() not in (".jpg", ".jpeg"):
            continue
        if out_dir:
            base_dir = next((p for p in inputs if p.is_dir() and src.is_relative_to(p)), None)
            rel = src.parent.relative_to(base_dir) if base_dir else Path()
            dst_dir = out_dir / rel
            dst_dir.mkdir(parents=True, exist_ok=True)
        else:
            dst_dir = src.parent

        img = Image.open(src)
        img = fit_with_white_bg(img, args.width, args.height)
        base = img.convert("L")

        if args.variant == "base":
            base = ImageOps.flip(base)
            base = ImageOps.mirror(base)
            base_path = dst_dir / (src.stem + "__BASE.g4")
            base_data = pack_g4(base)
            with open(base_path, "wb") as f:
                f.write(base_data)
            print(f"{src} -> {base_path} ({len(base_data)} bytes)")
        elif args.variant == "opt":
            opt = optimize_grayscale(base.copy())
            opt = ImageOps.flip(opt)
            opt = ImageOps.mirror(opt)
            opt_path = dst_dir / (src.stem + "__OPT.g4")
            opt_data = pack_g4(opt)
            with open(opt_path, "wb") as f:
                f.write(opt_data)
            print(f"{src} -> {opt_path} ({len(opt_data)} bytes)")
        elif args.variant == "opt-bayer":
            opt = optimize_grayscale(base.copy())
            opt = apply_bayer_dither(opt)
            opt = ImageOps.flip(opt)
            opt = ImageOps.mirror(opt)
            opt_path = dst_dir / (src.stem + "__OPT_BAYER.g4")
            opt_data = pack_g4(opt)
            with open(opt_path, "wb") as f:
                f.write(opt_data)
            print(f"{src} -> {opt_path} ({len(opt_data)} bytes)")
        elif args.variant == "opt-fs":
            opt = optimize_grayscale(base.copy())
            opt = apply_floyd_steinberg_dither(opt)
            opt = ImageOps.flip(opt)
            opt = ImageOps.mirror(opt)
            opt_path = dst_dir / (src.stem + "__OPT_FS.g4")
            opt_data = pack_g4(opt)
            with open(opt_path, "wb") as f:
                f.write(opt_data)
            print(f"{src} -> {opt_path} ({len(opt_data)} bytes)")
        else:
            opt = optimize_grayscale(base.copy())
            bayer = apply_bayer_dither(opt)
            fs = apply_floyd_steinberg_dither(opt)

            compare = Image.new("L", base.size, 255)
            quarter = base.width // 4
            x2 = quarter * 2
            x3 = quarter * 3

            compare.paste(base.crop((0, 0, quarter, base.height)), (0, 0))
            compare.paste(opt.crop((quarter, 0, x2, base.height)), (quarter, 0))
            compare.paste(bayer.crop((x2, 0, x3, base.height)), (x2, 0))
            compare.paste(fs.crop((x3, 0, base.width, base.height)), (x3, 0))

            compare = add_vertical_separators(compare, 4)
            compare = ImageOps.flip(compare)
            compare = ImageOps.mirror(compare)

            compare_path = dst_dir / (src.stem + "__COMPARE.g4")
            compare_data = pack_g4(compare)
            with open(compare_path, "wb") as f:
                f.write(compare_data)
            print(f"{src} -> {compare_path} ({len(compare_data)} bytes)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
