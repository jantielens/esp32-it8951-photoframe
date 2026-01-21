#!/usr/bin/env python3
"""Convert JPG images to .g4 and upload to an Azure Blob container via SAS."""

import argparse
import os
import sys
from pathlib import Path
from typing import Iterable
from urllib import request, parse, error

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


def convert_jpg_to_g4(src: Path, width: int, height: int, variant: str) -> bytes:
    img = Image.open(src)
    img = fit_with_white_bg(img, width, height)
    base = img.convert("L")

    if variant == "base":
        out = base
    elif variant == "opt":
        out = optimize_grayscale(base.copy())
    elif variant == "opt-bayer":
        out = apply_bayer_dither(optimize_grayscale(base.copy()))
    elif variant == "opt-fs":
        out = apply_floyd_steinberg_dither(optimize_grayscale(base.copy()))
    else:
        raise ValueError(f"Unsupported variant: {variant}")

    out = ImageOps.flip(out)
    out = ImageOps.mirror(out)
    return pack_g4(out)


def make_g4_name(src: Path, variant: str) -> str:
    suffix = {
        "base": "__BASE",
        "opt": "__OPT",
        "opt-bayer": "__OPT_BAYER",
        "opt-fs": "__OPT_FS",
    }[variant]
    return f"{src.stem}{suffix}.g4"


def build_blob_url(container_sas_url: str, blob_name: str) -> str:
    base, _, token = container_sas_url.partition("?")
    if not token:
        raise ValueError("SAS URL must include a query string token")
    container = base.rstrip("/")
    blob_path = parse.quote(blob_name, safe="/")
    return f"{container}/{blob_path}?{token}"


def upload_blob(url: str, payload: bytes, content_type: str = "application/octet-stream"):
    req = request.Request(url, method="PUT", data=payload)
    req.add_header("x-ms-blob-type", "BlockBlob")
    req.add_header("Content-Type", content_type)
    req.add_header("Content-Length", str(len(payload)))
    try:
        with request.urlopen(req, timeout=120) as resp:
            return resp.status, resp.read()
    except error.HTTPError as e:
        return e.code, e.read()


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert JPGs to .g4 and upload to Azure Blob Storage")
    parser.add_argument("input", nargs="+", help="Input file(s) or folder(s)")
    parser.add_argument("--sas-url", required=True, help="Azure Blob container SAS URL")
    parser.add_argument("--variant", choices=["base", "opt", "opt-bayer", "opt-fs"], default="opt-bayer")
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--prefix", default="", help="Optional blob prefix (e.g. 'photos/')")
    args = parser.parse_args()

    container_sas_url = args.sas_url
    prefix = args.prefix.lstrip("/")

    inputs = [Path(p).expanduser().resolve() for p in args.input]
    files = list(iter_images(inputs))
    if not files:
        print("No JPG files found.")
        return 1

    for src in files:
        if src.suffix.lower() not in (".jpg", ".jpeg"):
            continue
        g4_name = make_g4_name(src, args.variant)
        if prefix:
            blob_name = f"{prefix.rstrip('/')}/{g4_name}"
        else:
            blob_name = g4_name
        payload = convert_jpg_to_g4(src, args.width, args.height, args.variant)
        blob_url = build_blob_url(container_sas_url, blob_name)
        status, body = upload_blob(blob_url, payload, content_type="application/octet-stream")
        if status not in (200, 201):
            print(f"Upload failed for {blob_name} (status {status}): {body.decode(errors='ignore')}")
            return 1
        print(f"Uploaded {blob_name} ({len(payload)} bytes)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
