#!/usr/bin/env python3
import json
import os
import re
import tempfile
import threading
import urllib.request
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

BASE_URL = "https://eafc27.content.easports.com/fc/fltOnlineAssets/27A3C9F1-6B2E-4D7A-8C1F-2E9B5A4D6C7E/2027/"
ROSTER_URL = BASE_URL + "fc/fclive/genxtitle/rosterupdate.xml"
CACHE_DIR = Path(os.getenv("FC27_CACHE_DIR", "/opt/middleware/cache"))
STATIC_DIR = Path(__file__).parent
MAX_DATA_SIZE = 256 * 1024 * 1024
LOCK = threading.Lock()
FCBCHUNKS = b"\x46\x42\x43\x48\x55\x4E\x4B\x53\x01\x00"
BNRY = (
    b"\x42\x4E\x52\x59\x00\x00\x00\x02\x4C\x54\x4C\x45\x01\x01\x03\x00"
    b"\x00\x00\x63\x64\x73\x01\x00\x00\x00\x00\x01\x03\x00\x00\x00\x63\x64\x73"
)
FCBCHUNKS_HEADER_SIZE = 1126
SQUADS_BNRY_SIZE = 45985


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "FC27-Squad-Middleware/1.0"})
    with urllib.request.urlopen(request, timeout=45) as response:
        return response.read()


def decode_pack(source):
    if len(source) < 10:
        raise ValueError("invalid EA pack")
    size = int.from_bytes(source[2:5], "big")
    if size < 4 or size > MAX_DATA_SIZE:
        raise ValueError("invalid decoded size")
    output = bytearray(size)
    output[:4] = b"DB\x00\x08"
    input_pos, output_pos, trailing = 10, 4, 0
    while input_pos < len(source) and output_pos < size:
        control = source[input_pos]
        input_pos += 1
        trailing = control & 3
        if not control & 0x80:
            b1 = source[input_pos]
            input_pos += 1
            literals, length = control & 3, ((control >> 2) & 7) + 3
            distance = b1 + ((control & 0x60) << 3) + 1
        elif not control & 0x40:
            b2, b3 = source[input_pos:input_pos + 2]
            input_pos += 2
            literals, length = b2 >> 6, (control & 0x3F) + 4
            distance = ((b2 & 0x3F) << 8 | b3) + 1
        elif not control & 0x20:
            b2, b3, b4 = source[input_pos:input_pos + 3]
            input_pos += 3
            literals, length = control & 3, b4 + ((control & 0x0C) << 6) + 5
            distance = ((control & 0x10) << 12 | b2 << 8 | b3) + 1
        else:
            literals = (control & 0x1F) * 4 + 4
            if literals > 0x70:
                break
            output[output_pos:output_pos + literals] = source[input_pos:input_pos + literals]
            input_pos += literals
            output_pos += literals
            continue
        if input_pos + literals > len(source) or output_pos + literals + length > size or distance > output_pos:
            raise ValueError("corrupt EA pack")
        output[output_pos:output_pos + literals] = source[input_pos:input_pos + literals]
        input_pos += literals
        output_pos += literals
        for index in range(length):
            output[output_pos + index] = output[output_pos - distance + index]
        output_pos += length
    if trailing and output_pos < size:
        output[output_pos:output_pos + trailing] = source[input_pos:input_pos + trailing]
    if output_pos != size:
        raise ValueError("truncated EA pack")
    return bytes(output)


def wrap_squads(db, version):
    prefix = bytearray(FCBCHUNKS_HEADER_SIZE)
    position = 0
    prefix[position:position + len(FCBCHUNKS)] = FCBCHUNKS
    position += len(FCBCHUNKS)
    prefix[position:position + 4] = (FCBCHUNKS_HEADER_SIZE - position - 8).to_bytes(4, "little")
    position += 4
    file_size = 48 + 4 + len(db) + SQUADS_BNRY_SIZE
    prefix[position:position + 4] = file_size.to_bytes(4, "little")
    position += 4
    in_game_name = ("EA_" + version).encode()[:40]
    prefix[position:position + len(in_game_name)] = in_game_name
    position += len(in_game_name) + 7
    prefix[position:position + len(b"Aranaktu")] = b"Aranaktu"

    header = bytearray(48)
    save_type = b"SaveType_Squads\x00"
    header[:len(save_type)] = save_type
    return bytes(prefix) + bytes(header) + (len(db) + SQUADS_BNRY_SIZE).to_bytes(4, "little") + db + BNRY + b"\0" * (SQUADS_BNRY_SIZE - len(BNRY))


def latest_squad_info():
    xml = fetch(ROSTER_URL).decode("utf-8")
    match = re.search(r'<squadInfo platform="ps5">.*?<dbMajorLoc>([^<]+)</dbMajorLoc>', xml, re.S)
    if not match:
        raise ValueError("PS5 roster location missing")
    location = match.group(1)
    date = re.search(r"squads_(\d{8})_", location)
    if not date:
        raise ValueError("PS5 roster date missing")
    return "Squads" + date.group(1) + "000000", location


def latest_squad():
    with LOCK:
        version, location = latest_squad_info()
        destination = CACHE_DIR / (version + ".raw")
        CACHE_DIR.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            decoded = decode_pack(fetch(BASE_URL + location))
            with tempfile.NamedTemporaryFile(dir=CACHE_DIR, delete=False) as temp:
                temp.write(decoded)
                temp.flush()
                os.fsync(temp.fileno())
                os.replace(temp.name, destination)
        return version, wrap_squads(destination.read_bytes(), version)
class Handler(BaseHTTPRequestHandler):
    def send_bytes(self, status, body, content_type="application/json", version=""):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Expose-Headers", "X-FC27-Version")
        if version:
            self.send_header("X-FC27-Version", version)
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(HTTPStatus.NO_CONTENT)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-FC27-Version")
        self.end_headers()

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/" or path == "/index.html":
            self.send_bytes(HTTPStatus.OK, (STATIC_DIR / "index.html").read_bytes(), "text/html; charset=utf-8")
            return
        if path == "/logo.svg":
            self.send_bytes(HTTPStatus.OK, (STATIC_DIR / "logo.svg").read_bytes(), "image/svg+xml")
            return
        if path not in ("/v1/squads/check", "/v1/squads/download"):
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        try:
            current = self.headers.get("X-FC27-Version", "")
            if path == "/v1/squads/check":
                version, _ = latest_squad_info()
                self.send_bytes(HTTPStatus.OK, json.dumps({"version": version, "update": current != version}).encode(), version=version)
                return
            version, squads = latest_squad()
            self.send_bytes(HTTPStatus.OK, squads, "application/octet-stream", version)
        except Exception as error:
            self.send_bytes(HTTPStatus.BAD_GATEWAY, json.dumps({"error": str(error)}).encode())

    def do_POST(self):
        self.send_error(HTTPStatus.METHOD_NOT_ALLOWED)

    def log_message(self, format, *args):
        print("%s - %s" % (self.address_string(), format % args))


if __name__ == "__main__":
    address = (os.getenv("FC27_HOST", "127.0.0.1"), int(os.getenv("FC27_PORT", "3033")))
    ThreadingHTTPServer(address, Handler).serve_forever()
