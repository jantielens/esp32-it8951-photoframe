import json
import os
import re
import uuid
from datetime import datetime, timedelta
from io import BytesIO
from pathlib import Path
from typing import Dict, List, Optional, Tuple
from urllib import error, parse, request

from fastapi import FastAPI, File, Form, Request, UploadFile
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.templating import Jinja2Templates
from PIL import Image, ImageEnhance, ImageFilter, ImageOps

DEFAULT_WIDTH = 1872
DEFAULT_HEIGHT = 1404
DEFAULT_VARIANT = "opt-bayer"

BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = Path(os.getenv("CONFIG_PATH", BASE_DIR / "config.json"))
ENV = os.getenv("ENV", "prod").lower()
DEV_USER = os.getenv("DEV_USER", "dev@example.com")

app = FastAPI()
templates = Jinja2Templates(directory=str(BASE_DIR / "templates"))


def load_config() -> Dict:
    if not CONFIG_PATH.exists():
        raise RuntimeError(f"Config file not found: {CONFIG_PATH}")
    with CONFIG_PATH.open("r", encoding="utf-8") as f:
        return json.load(f)


def get_current_user(request: Request) -> Optional[str]:
    if ENV == "dev":
        return DEV_USER
    return request.headers.get("X-MS-CLIENT-PRINCIPAL-NAME")


def get_allowed_devices(config: Dict, user: str) -> List[str]:
    users = config.get("users", {})
    return users.get(user) or users.get("*") or []


def slugify(value: str) -> str:
    value = value.strip().replace(" ", "_")
    return re.sub(r"[^a-zA-Z0-9._-]", "", value) or "image"


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


def convert_jpg_to_g4_image(img: Image.Image, width: int, height: int, variant: str) -> bytes:
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


def make_perm_g4_name() -> str:
    guid = str(uuid.uuid4())
    upload_ts = datetime.utcnow().strftime("%Y%m%dT%H%M%SZ")
    return f"perm/{upload_ts}__{guid}.g4"


def make_temp_g4_name(expires_at: datetime) -> str:
    guid = str(uuid.uuid4())
    upload_ts = datetime.utcnow().strftime("%Y%m%dT%H%M%SZ")
    expires_ts = expires_at.strftime("%Y%m%dT%H%M%SZ")
    return f"temp/{expires_ts}__{upload_ts}__{guid}.g4"


def build_blob_url(container_sas_url: str, blob_name: str) -> str:
    base, _, token = container_sas_url.partition("?")
    if not token:
        raise ValueError("SAS URL must include a query string token")
    container = base.rstrip("/")
    blob_path = parse.quote(blob_name, safe="/")
    return f"{container}/{blob_path}?{token}"


def upload_blob(url: str, payload: bytes, content_type: str = "application/octet-stream") -> Tuple[int, bytes]:
    req = request.Request(url, method="PUT", data=payload)
    req.add_header("x-ms-blob-type", "BlockBlob")
    req.add_header("Content-Type", content_type)
    req.add_header("Content-Length", str(len(payload)))
    try:
        with request.urlopen(req, timeout=120) as resp:
            return resp.status, resp.read()
    except error.HTTPError as e:
        return e.code, e.read()


def get_device_sas_url(config: Dict, device_id: str) -> Optional[str]:
    devices = config.get("devices", {})
    device = devices.get(device_id)
    if not device:
        return None
    return device.get("container_sas_url")


def build_device_list(config: Dict, allowed: List[str]) -> List[Dict[str, str]]:
    devices = config.get("devices", {})
    result = []
    for device_id in allowed:
        if device_id in devices:
            result.append({"id": device_id})
    return result


@app.get("/", response_class=HTMLResponse)
def index(request: Request):
    config = load_config()
    user = get_current_user(request)
    if not user:
        return templates.TemplateResponse(
            "index.html",
            {"request": request, "user": None, "devices": [], "error": "Not authenticated"},
        )

    allowed = get_allowed_devices(config, user)
    devices = build_device_list(config, allowed)
    if not devices:
        return templates.TemplateResponse(
            "index.html",
            {"request": request, "user": user, "devices": [], "error": "No devices assigned"},
        )

    return templates.TemplateResponse(
        "index.html",
        {"request": request, "user": user, "devices": devices, "error": None},
    )


@app.post("/upload")
async def upload(
    request: Request,
    device_id: str = Form(...),
    queue: str = Form("perm"),
    ttl_hours: Optional[int] = Form(None),
    file: UploadFile = File(...),
):
    config = load_config()
    user = get_current_user(request)
    if not user:
        return JSONResponse(status_code=401, content={"status": "error", "error": "Not authenticated"})

    allowed = get_allowed_devices(config, user)
    devices = build_device_list(config, allowed)
    if device_id not in allowed:
        return JSONResponse(status_code=403, content={"status": "error", "error": "Device not allowed"})

    if not file or not file.filename:
        return JSONResponse(status_code=400, content={"status": "error", "error": "No file uploaded"})

    if file.content_type not in ("image/jpeg", "image/jpg") and not file.filename.lower().endswith((".jpg", ".jpeg")):
        return JSONResponse(status_code=400, content={"status": "error", "error": "Only JPG files are supported"})

    try:
        data = await file.read()
        img = Image.open(BytesIO(data))
        payload = convert_jpg_to_g4_image(img, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_VARIANT)
    except Exception as exc:
        return JSONResponse(status_code=400, content={"status": "error", "error": f"Conversion failed: {exc}"})

    container_sas_url = get_device_sas_url(config, device_id)
    if not container_sas_url:
        return JSONResponse(status_code=404, content={"status": "error", "error": "Device not found"})

    queue = (queue or "perm").lower()
    if queue not in ("perm", "temp"):
        return JSONResponse(status_code=400, content={"status": "error", "error": "Invalid queue"})

    if queue == "temp":
        ttl = ttl_hours if ttl_hours is not None else 24
        if ttl <= 0 or ttl > 24 * 30:
            return JSONResponse(status_code=400, content={"status": "error", "error": "Invalid TTL"})
        expires_at = datetime.utcnow() + timedelta(hours=ttl)
        blob_name = make_temp_g4_name(expires_at)
    else:
        blob_name = make_perm_g4_name()
    blob_url = build_blob_url(container_sas_url, blob_name)
    status, body = upload_blob(blob_url, payload, content_type="application/octet-stream")
    if status not in (200, 201):
        detail = body.decode(errors="ignore")
        return JSONResponse(
            status_code=502,
            content={"status": "error", "error": f"Upload failed (status {status}): {detail}"},
        )

    return JSONResponse(
        status_code=200,
        content={"status": "ok", "message": f"Uploaded {blob_name} ({len(payload)} bytes)"},
    )