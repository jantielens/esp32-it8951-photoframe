#!/usr/bin/env python3
"""Sync JPG images to device SD as .g4 files via REST API."""

import argparse
import base64
import mimetypes
import os
import sys
import time
from io import BytesIO
from pathlib import Path
from typing import Iterable, Optional
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


def build_auth_header(auth: Optional[str]) -> dict:
    if not auth:
        return {}
    token = base64.b64encode(auth.encode("utf-8")).decode("ascii")
    return {"Authorization": f"Basic {token}"}


def request_json(url: str, method: str = "GET", data: Optional[bytes] = None, headers: Optional[dict] = None):
    req = request.Request(url, method=method, data=data)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with request.urlopen(req, timeout=60) as resp:
            body = resp.read()
            return resp.status, body
    except error.HTTPError as e:
        return e.code, e.read()


def parse_job_id(body: bytes) -> Optional[int]:
    try:
        import json
        payload = json.loads(body.decode("utf-8"))
        job_id = payload.get("job_id")
        if isinstance(job_id, int):
            return job_id
    except Exception:
        return None
    return None


def wait_job(device: str, job_id: int, headers: Optional[dict] = None, timeout_s: int = 120):
    start = time.time()
    while time.time() - start < timeout_s:
        status, body = request_json(f"{device}/api/sd/jobs?{parse.urlencode({'id': job_id})}", headers=headers)
        if status != 200:
            return status, body
        try:
            import json
            payload = json.loads(body.decode("utf-8"))
            state = payload.get("state")
            if state == "done":
                return 200, body
            if state == "error":
                return 500, body
        except Exception:
            return 500, body
        time.sleep(0.5)
    return 504, b"{\"success\":false,\"message\":\"Job timeout\"}"


def request_multipart(url: str, field: str, filename: str, payload: bytes, headers: Optional[dict] = None):
    boundary = "----esp32photoframeboundary"
    content_type = f"multipart/form-data; boundary={boundary}"
    body = BytesIO()
    body.write(f"--{boundary}\r\n".encode("utf-8"))
    body.write(f"Content-Disposition: form-data; name=\"{field}\"; filename=\"{filename}\"\r\n".encode("utf-8"))
    body.write(f"Content-Type: {mimetypes.guess_type(filename)[0] or 'application/octet-stream'}\r\n\r\n".encode("utf-8"))
    body.write(payload)
    body.write(f"\r\n--{boundary}--\r\n".encode("utf-8"))
    data = body.getvalue()

    req = request.Request(url, method="POST", data=data)
    req.add_header("Content-Type", content_type)
    req.add_header("Content-Length", str(len(data)))
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with request.urlopen(req, timeout=120) as resp:
            return resp.status, resp.read()
    except error.HTTPError as e:
        return e.code, e.read()


def upload_with_retry(url: str, filename: str, payload: bytes, headers: Optional[dict] = None,
                      retries: int = 3, delay_s: float = 1.0):
    for attempt in range(retries + 1):
        status, body = request_multipart(url, "file", filename, payload, headers=headers)
        if status == 202:
            return status, body
        if status >= 500 and attempt < retries:
            time.sleep(delay_s)
            continue
        return status, body
    return status, body


def main() -> int:
    parser = argparse.ArgumentParser(description="Sync JPGs to device SD as .g4")
    parser.add_argument("input", nargs="+", help="Input file(s) or folder(s)")
    parser.add_argument("--device", required=True, help="Device base URL (e.g. http://esp32.local)")
    parser.add_argument("--variant", choices=["base", "opt", "opt-bayer", "opt-fs"], default="opt-bayer")
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--auth", default=None, help="Basic auth user:pass")
    args = parser.parse_args()

    device = args.device.rstrip("/")
    auth_header = build_auth_header(args.auth)

    # Pause rendering
    pause_status, _ = request_json(f"{device}/api/render/pause", method="POST", headers=auth_header)
    if pause_status not in (200, 204):
        print(f"Failed to pause rendering (status {pause_status})")
        return 1

    try:
        # Delete existing .g4 files
        status, body = request_json(f"{device}/api/sd/images", method="GET", headers=auth_header)
        if status != 202:
            print(f"Failed to queue list images (status {status}): {body.decode(errors='ignore')}")
            return 1
        job_id = parse_job_id(body)
        if not job_id:
            print("Failed to parse list job id")
            return 1
        status, body = wait_job(device, job_id, headers=auth_header, timeout_s=120)
        if status != 200:
            print(f"Failed to list images (status {status}): {body.decode(errors='ignore')}")
            return 1
        data = body.decode("utf-8")
        names = []
        try:
            import json
            parsed = json.loads(data)
            names = parsed.get("files", [])
        except Exception as exc:
            print(f"Failed to parse image list: {exc}")
            return 1

        for name in names:
            del_status, del_body = request_json(
                f"{device}/api/sd/images?{parse.urlencode({'name': name})}",
                method="DELETE",
                headers=auth_header,
            )
            if del_status != 202:
                print(f"Delete queue failed for {name} (status {del_status}): {del_body.decode(errors='ignore')}")
                return 1
            del_job_id = parse_job_id(del_body)
            if not del_job_id:
                print(f"Delete job id missing for {name}")
                return 1
            del_status, del_body = wait_job(device, del_job_id, headers=auth_header, timeout_s=120)
            if del_status != 200:
                print(f"Delete failed for {name} (status {del_status}): {del_body.decode(errors='ignore')}")
                return 1

        # Convert and upload
        inputs = [Path(p).expanduser().resolve() for p in args.input]
        files = list(iter_images(inputs))
        if not files:
            print("No JPG files found.")
            return 1

        for src in files:
            if src.suffix.lower() not in (".jpg", ".jpeg"):
                continue
            g4_name = make_g4_name(src, args.variant)
            payload = convert_jpg_to_g4(src, args.width, args.height, args.variant)
            up_status, up_body = upload_with_retry(
                f"{device}/api/sd/images",
                g4_name,
                payload,
                headers=auth_header,
            )
            if up_status != 202:
                print(f"Upload failed for {g4_name} (status {up_status}): {up_body.decode(errors='ignore')}")
                return 1
            up_job_id = parse_job_id(up_body)
            if not up_job_id:
                print(f"Upload job id missing for {g4_name}")
                return 1
            done_status, done_body = wait_job(device, up_job_id, headers=auth_header, timeout_s=240)
            if done_status != 200:
                print(f"Upload failed for {g4_name} (status {done_status}): {done_body.decode(errors='ignore')}")
                return 1
            print(f"Uploaded {g4_name} ({len(payload)} bytes)")
    finally:
        request_json(f"{device}/api/render/resume", method="POST", headers=auth_header)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
