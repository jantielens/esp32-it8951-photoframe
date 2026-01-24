import json
import os
import re
import uuid
from datetime import datetime, timedelta
from io import BytesIO
from pathlib import Path
from typing import Dict, List, Optional, Tuple
from urllib import error, parse, request as urllib_request

from fastapi import FastAPI, File, Form, Request, UploadFile
from fastapi.responses import HTMLResponse, JSONResponse, Response
from fastapi.templating import Jinja2Templates
from PIL import Image, ImageEnhance, ImageFilter, ImageOps

import xml.etree.ElementTree as ET

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


def convert_jpg_to_processed_image(img: Image.Image, width: int, height: int, variant: str) -> Image.Image:
    """Return the final processed grayscale image (orientation + dither applied) used for G4 packing.

    This is used to generate archive previews that match the device output.
    """

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
    return out


def _img_to_jpeg_bytes(img: Image.Image, *, quality: int = 85) -> bytes:
    bio = BytesIO()
    img.save(bio, format="JPEG", quality=quality, optimize=True)
    return bio.getvalue()


def _make_thumb_original(img: Image.Image, size: int = 100) -> Image.Image:
    """Letterboxed thumb for the *original* upload.

    Keeps the image unprocessed (no device-specific flip/mirror/dither). We do apply
    EXIF transpose so the thumbnail matches how browsers typically display photos.
    """

    img = ImageOps.exif_transpose(img)
    img = img.convert("RGB")
    target = (size, size)
    thumb = ImageOps.contain(img, target)
    canvas = Image.new("RGB", target, color=(255, 255, 255))
    x = (size - thumb.size[0]) // 2
    y = (size - thumb.size[1]) // 2
    canvas.paste(thumb, (x, y))
    return canvas


def derive_all_names(g4_path: str) -> Tuple[str, str, str, str]:
    """Map a queue g4 name to its all/* artifact names (g4 + jpg + thumb + meta)."""

    if not g4_path.lower().endswith(".g4"):
        raise ValueError("g4_path must end with .g4")

    if g4_path.startswith("queue-temporary/"):
        rest = g4_path[len("queue-temporary/") :]
        base = f"all/temporary/{rest[:-3]}"
    elif g4_path.startswith("queue-permanent/"):
        rest = g4_path[len("queue-permanent/") :]
        base = f"all/permanent/{rest[:-3]}"
    else:
        raise ValueError("g4_path must start with queue-temporary/ or queue-permanent/")

    all_g4 = f"{base}.g4"
    full_jpg = f"{base}.jpg"
    thumb_jpg = f"{base}__thumb.jpg"
    meta_json = f"{base}.json"
    return all_g4, full_jpg, thumb_jpg, meta_json


def make_perm_g4_name() -> str:
    guid = str(uuid.uuid4())
    upload_ts = datetime.utcnow().strftime("%Y%m%dT%H%M%SZ")
    return f"queue-permanent/{upload_ts}__{guid}.g4"


def make_temp_g4_name(expires_at: datetime) -> str:
    guid = str(uuid.uuid4())
    upload_ts = datetime.utcnow().strftime("%Y%m%dT%H%M%SZ")
    expires_ts = expires_at.strftime("%Y%m%dT%H%M%SZ")
    return f"queue-temporary/{expires_ts}__{upload_ts}__{guid}.g4"


def build_blob_url(container_sas_url: str, blob_name: str) -> str:
    base, _, token = container_sas_url.partition("?")
    if not token:
        raise ValueError("SAS URL must include a query string token")
    container = base.rstrip("/")
    blob_path = parse.quote(blob_name, safe="/")
    return f"{container}/{blob_path}?{token}"


def upload_blob(url: str, payload: bytes, content_type: str = "application/octet-stream") -> Tuple[int, bytes]:
    req = urllib_request.Request(url, method="PUT", data=payload)
    req.add_header("x-ms-blob-type", "BlockBlob")
    req.add_header("Content-Type", content_type)
    req.add_header("Content-Length", str(len(payload)))
    try:
        with urllib_request.urlopen(req, timeout=120) as resp:
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
    queue: str = Form("queue-permanent"),
    ttl_hours: Optional[int] = Form(None),
    caption: Optional[str] = Form(None),
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
        original_bytes = await file.read()
        img = Image.open(BytesIO(original_bytes))
        processed = convert_jpg_to_processed_image(img, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_VARIANT)
        payload = pack_g4(processed)
    except Exception as exc:
        return JSONResponse(status_code=400, content={"status": "error", "error": f"Conversion failed: {exc}"})

    container_sas_url = get_device_sas_url(config, device_id)
    if not container_sas_url:
        return JSONResponse(status_code=404, content={"status": "error", "error": "Device not found"})

    queue = (queue or "queue-permanent").lower()
    if queue not in ("queue-permanent", "queue-temporary"):
        return JSONResponse(status_code=400, content={"status": "error", "error": "Invalid queue"})

    expires_at: Optional[datetime] = None
    if queue == "queue-temporary":
        ttl = ttl_hours if ttl_hours is not None else 24
        if ttl <= 0 or ttl > 24 * 30:
            return JSONResponse(status_code=400, content={"status": "error", "error": "Invalid TTL"})
        expires_at = datetime.utcnow() + timedelta(hours=ttl)
        blob_name = make_temp_g4_name(expires_at)
    else:
        blob_name = make_perm_g4_name()

    # all/* outputs derived from the queue g4 path.
    all_g4_name, full_jpg_name, thumb_jpg_name, meta_json_name = derive_all_names(blob_name)

    # Store the *original uploaded file* in all/ (unprocessed).
    # This is intentional: all/ is for human previews/auditing, not for matching device output.
    full_jpg_bytes = original_bytes
    thumb_jpg_bytes = _img_to_jpeg_bytes(_make_thumb_original(Image.open(BytesIO(original_bytes)), 100), quality=75)
    uploaded_at_utc = datetime.utcnow().replace(microsecond=0).isoformat() + "Z"
    expires_at_utc = expires_at.replace(microsecond=0).isoformat() + "Z" if expires_at else None
    meta = {
        "v": 1,
        "g4_path": blob_name,
        "original_filename": file.filename,
        "caption": (caption or "").strip(),
        "uploader": user,
        "uploaded_at_utc": uploaded_at_utc,
    }
    if expires_at_utc:
        meta["expires_at_utc"] = expires_at_utc
    meta_bytes = json.dumps(meta, separators=(",", ":"), ensure_ascii=False).encode("utf-8")

    # Upload g4 to all/ (truth) and to queue (delivery).
    all_g4_url = build_blob_url(container_sas_url, all_g4_name)
    status, body = upload_blob(all_g4_url, payload, content_type="application/octet-stream")
    if status not in (200, 201):
        detail = body.decode(errors="ignore")
        return JSONResponse(
            status_code=502,
            content={"status": "error", "error": f"Upload failed (status {status}): {detail}"},
        )

    queue_url = build_blob_url(container_sas_url, blob_name)
    status, body = upload_blob(queue_url, payload, content_type="application/octet-stream")
    if status not in (200, 201):
        detail = body.decode(errors="ignore")
        return JSONResponse(
            status_code=502,
            content={"status": "error", "error": f"Queue upload failed (status {status}): {detail}"},
        )

    # Best-effort all/ uploads (thumb/full/meta). These are non-critical for device operation,
    # but are required for previews.
    for name, content, ctype in (
        (full_jpg_name, full_jpg_bytes, "image/jpeg"),
        (thumb_jpg_name, thumb_jpg_bytes, "image/jpeg"),
        (meta_json_name, meta_bytes, "application/json"),
    ):
        url = build_blob_url(container_sas_url, name)
        s, b = upload_blob(url, content, content_type=ctype)
        if s not in (200, 201):
            detail = b.decode(errors="ignore")
            return JSONResponse(
                status_code=502,
                content={"status": "error", "error": f"All upload failed for {name} (status {s}): {detail}"},
            )

    return JSONResponse(
        status_code=200,
        content={
            "status": "ok",
            "message": f"Uploaded {blob_name} ({len(payload)} bytes)",
            "g4_path": blob_name,
        },
    )


def _is_valid_g4_name(name: str) -> bool:
    if not name or len(name) > 127:
        return False
    if "\\" in name or ".." in name:
        return False
    lower = name.lower()
    if not lower.endswith(".g4"):
        return False
    if name.count("/") != 1:
        return False
    return name.startswith("queue-permanent/") or name.startswith("queue-temporary/")


def _parse_azure_list(xml_bytes: bytes) -> Tuple[List[str], Optional[str]]:
    # Azure container list response is XML; parse <Name> and <NextMarker>.
    root = ET.fromstring(xml_bytes)
    names: List[str] = []

    # Namespace may be present; use wildcard.
    for elem in root.findall(".//{*}Name"):
        if elem.text:
            names.append(elem.text)

    next_marker_elem = root.find(".//{*}NextMarker")
    next_marker = next_marker_elem.text if (next_marker_elem is not None and next_marker_elem.text) else None
    return names, next_marker


def list_blobs_prefix(container_sas_url: str, prefix: str, *, max_results: int = 200) -> List[str]:
    base, _, token = container_sas_url.partition("?")
    if not token:
        raise ValueError("SAS URL must include a query string token")
    container = base.rstrip("/")

    marker: Optional[str] = None
    out: List[str] = []
    while True:
        q = token + "&restype=container&comp=list" + f"&maxresults={max_results}" + "&prefix=" + parse.quote(prefix, safe="")
        if marker:
            q += "&marker=" + parse.quote(marker, safe="")
        url = f"{container}?{q}"
        with urllib_request.urlopen(url, timeout=10) as resp:
            body = resp.read()
        names, marker = _parse_azure_list(body)
        out.extend(names)
        if not marker:
            break
    return out


def _derive_g4_name_from_archive_thumb(thumb_blob_name: str) -> Optional[str]:
    """Map all/ thumb blob name to its logical queue g4 name.

    Example: all/permanent/<id>__thumb.jpg -> queue-permanent/<id>.g4
    """

    lower = thumb_blob_name.lower()
    if not lower.endswith("__thumb.jpg"):
        return None
    if not thumb_blob_name.startswith("all/"):
        return None

    # Strip leading "all/".
    rest = thumb_blob_name[len("all/") :]
    if rest.startswith("permanent/"):
        base = rest[: -len("__thumb.jpg")]
        return "queue-permanent/" + base[len("permanent/") :] + ".g4"
    if rest.startswith("temporary/"):
        base = rest[: -len("__thumb.jpg")]
        return "queue-temporary/" + base[len("temporary/") :] + ".g4"
    return None


def _temp_expiry_status(g4_name: str, now: datetime) -> Tuple[Optional[str], Optional[bool]]:
    if not g4_name.startswith("queue-temporary/"):
        return None, None

    expires = None
    expired = None
    m = re.match(r"^queue-temporary/(?P<expires>\d{8}T\d{6}Z)__", g4_name)
    if m:
        try:
            expires_dt = datetime.strptime(m.group("expires"), "%Y%m%dT%H%M%SZ")
            expires_dt = expires_dt.replace(microsecond=0)
            expires = expires_dt.isoformat() + "Z"
            expired = now >= expires_dt
        except Exception:
            expires = None
            expired = None
    return expires, expired


@app.get("/api/list")
def api_list(request: Request, device_id: str):
    config = load_config()
    user = get_current_user(request)
    if not user:
        return JSONResponse(status_code=401, content={"status": "error", "error": "Not authenticated"})
    allowed = get_allowed_devices(config, user)
    if device_id not in allowed:
        return JSONResponse(status_code=403, content={"status": "error", "error": "Device not allowed"})

    container_sas_url = get_device_sas_url(config, device_id)
    if not container_sas_url:
        return JSONResponse(status_code=404, content={"status": "error", "error": "Device not found"})

    try:
        perm = [n for n in list_blobs_prefix(container_sas_url, "queue-permanent/") if n.lower().endswith(".g4")]
        temp = [n for n in list_blobs_prefix(container_sas_url, "queue-temporary/") if n.lower().endswith(".g4")]

        # all/ artifacts are stored under all/<kind>/<id>__thumb.jpg.
        # We list thumbs only (fast, small) and derive the corresponding g4 name.
        archive_perm = [
            n
            for n in list_blobs_prefix(container_sas_url, "all/permanent/")
            if n.lower().endswith("__thumb.jpg")
        ]
        archive_temp = [
            n
            for n in list_blobs_prefix(container_sas_url, "all/temporary/")
            if n.lower().endswith("__thumb.jpg")
        ]
    except Exception as exc:
        return JSONResponse(status_code=502, content={"status": "error", "error": f"List failed: {exc}"})

    # Status badge for temp: active vs expired.
    now = datetime.utcnow()
    temp_items = []
    for n in temp:
        expires_at_utc, expired = _temp_expiry_status(n, now)
        temp_items.append({"name": n, "expires_at_utc": expires_at_utc, "expired": expired})

    # Build merged "device twin" list.
    queued = set(perm) | set(temp)
    archived: set[str] = set()
    for blob in archive_perm + archive_temp:
        g4_name = _derive_g4_name_from_archive_thumb(blob)
        if g4_name:
            archived.add(g4_name)

    all_items = queued | archived
    merged = []
    for name in all_items:
        queue_kind = "temporary" if name.startswith("queue-temporary/") else "permanent"
        expires_at_utc, expired = _temp_expiry_status(name, now)
        queued_flag = name in queued
        archived_flag = name in archived
        merged.append(
            {
                "name": name,
                "queue": queue_kind,
                "queued": queued_flag,
                "archived": archived_flag,
                # UI semantics: "on device" means present in all/ but not currently queued.
                "on_device": (archived_flag and not queued_flag),
                "expires_at_utc": expires_at_utc,
                "expired": expired,
            }
        )

    # Sort (stable): newest-ish by name (desc), then queued-first, then temp-before-perm.
    merged.sort(key=lambda it: it.get("name") or "", reverse=True)
    merged.sort(key=lambda it: 0 if it.get("queued") else 1)
    merged.sort(key=lambda it: 0 if it.get("queue") == "temporary" else 1)

    return JSONResponse(
        status_code=200,
        content={
            "status": "ok",
            "perm": [{"name": n} for n in perm],
            "temp": temp_items,
            "items": merged,
        },
    )


@app.get("/thumb")
def get_thumb(request: Request, device_id: str, name: str):
    config = load_config()
    user = get_current_user(request)
    if not user:
        return JSONResponse(status_code=401, content={"status": "error", "error": "Not authenticated"})
    allowed = get_allowed_devices(config, user)
    if device_id not in allowed:
        return JSONResponse(status_code=403, content={"status": "error", "error": "Device not allowed"})

    if not _is_valid_g4_name(name):
        return JSONResponse(status_code=400, content={"status": "error", "error": "Invalid name"})

    container_sas_url = get_device_sas_url(config, device_id)
    if not container_sas_url:
        return JSONResponse(status_code=404, content={"status": "error", "error": "Device not found"})

    _, _, thumb_jpg_name, _ = derive_all_names(name)
    url = build_blob_url(container_sas_url, thumb_jpg_name)
    try:
        # Thumbnails are small, but Azure can still be slow (DNS/TLS/cold paths). Keep this reasonable.
        with urllib_request.urlopen(url, timeout=15) as resp:
            data = resp.read()
        return Response(content=data, media_type="image/jpeg", headers={"Cache-Control": "public, max-age=300"})
    except error.HTTPError as e:
        body = b""
        try:
            body = e.read()
        except Exception:
            body = b""
        detail = (body[:256].decode(errors="ignore") if body else "")
        print(f"/thumb upstream HTTPError device_id={device_id} name={name} blob={thumb_jpg_name} code={e.code} detail={detail}")
        if e.code == 404:
            return Response(content=b"", status_code=404)
        # Surface upstream status to make permission issues obvious.
        return Response(content=b"", status_code=502, headers={"X-Upstream-Status": str(e.code)})
    except Exception as exc:
        print(f"/thumb upstream error device_id={device_id} name={name} blob={thumb_jpg_name} err={exc}")
        return Response(content=b"", status_code=502)


@app.get("/meta")
def get_meta(request: Request, device_id: str, name: str):
    config = load_config()
    user = get_current_user(request)
    if not user:
        return JSONResponse(status_code=401, content={"status": "error", "error": "Not authenticated"})
    allowed = get_allowed_devices(config, user)
    if device_id not in allowed:
        return JSONResponse(status_code=403, content={"status": "error", "error": "Device not allowed"})

    if not _is_valid_g4_name(name):
        return JSONResponse(status_code=400, content={"status": "error", "error": "Invalid name"})

    container_sas_url = get_device_sas_url(config, device_id)
    if not container_sas_url:
        return JSONResponse(status_code=404, content={"status": "error", "error": "Device not found"})

    _, _, _, meta_json_name = derive_all_names(name)
    url = build_blob_url(container_sas_url, meta_json_name)
    try:
        with urllib_request.urlopen(url, timeout=15) as resp:
            data = resp.read()
        return Response(content=data, media_type="application/json", headers={"Cache-Control": "public, max-age=300"})
    except error.HTTPError as e:
        body = b""
        try:
            body = e.read()
        except Exception:
            body = b""
        detail = (body[:256].decode(errors="ignore") if body else "")
        print(f"/meta upstream HTTPError device_id={device_id} name={name} blob={meta_json_name} code={e.code} detail={detail}")
        if e.code == 404:
            return Response(content=b"{}", status_code=404, media_type="application/json")
        return Response(
            content=b"{}",
            status_code=502,
            media_type="application/json",
            headers={"X-Upstream-Status": str(e.code)},
        )
    except Exception as exc:
        print(f"/meta upstream error device_id={device_id} name={name} blob={meta_json_name} err={exc}")
        return Response(content=b"{}", status_code=502, media_type="application/json")