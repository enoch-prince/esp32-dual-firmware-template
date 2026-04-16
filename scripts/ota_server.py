#!/usr/bin/env python3
"""
Local HTTPS OTA server for the ESP32 dual-firmware project.

Features
--------
* Serves firmware binaries + JSON manifests over HTTPS (TLS pinning-safe)
* Auto-computes SHA-256 and signs with ECDSA-P256 on every upload
  (raw r||s format required by PSA psa_verify_hash on the device)
* Web dashboard to upload binaries, update versions, and trigger OTA on a device
* --setup flag generates keys/certs and copies them into firmware_a/certs/ and
  firmware_b/certs/. Re-running --setup regenerates only the TLS cert (the ECDSA
  signing key is preserved unless --force-new-key is also passed).

Requirements
------------
    pip install flask cryptography

Usage
-----
    # First-time setup (from WSL2 or Linux — auto-detects LAN IP for TLS cert SAN)
    python3 scripts/ota_server.py --setup

    # First-time setup from Windows — pass the server's LAN IP explicitly
    python  scripts/ota_server.py --setup --host 192.168.1.x

    # Setup from Windows when firmware dirs are on WSL2 (copies certs automatically)
    python  scripts/ota_server.py --setup --host 192.168.1.x ^
        --project-root "\\\\wsl.localhost\\Ubuntu\\home\\epk\\sample_project"

    # Re-run setup after moving to a new IP (preserves signing key, new TLS cert only)
    python3 scripts/ota_server.py --setup --host <new-ip>

    # Replace the signing key as well (previously signed binaries must be re-uploaded)
    python3 scripts/ota_server.py --setup --force-new-key

    # Start server (binds to all interfaces; LAN IP shown in startup output)
    python3 scripts/ota_server.py               # WSL2 / Linux
    python  scripts/ota_server.py               # Windows

    # Start on a specific interface or port
    python3 scripts/ota_server.py --host 0.0.0.0 --port 8443

Manifest format served at  GET /<variant>/manifest.json
-------------------------------------------------------
{
  "firmware_a": {
    "version":   "0.2.0",
    "url":       "https://<server-ip>:8443/fw_a/sample_project.bin",
    "sha256":    "<lowercase hex SHA-256 of binary>",
    "signature": "<base64(raw r||s, 64 bytes) ECDSA-P256 over SHA-256 of binary>"
  }
}
"""

import argparse
import base64
import hashlib
import hmac
import ipaddress
import json
import os
import shutil
import socket
import ssl
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

# ── Dependency checks ──────────────────────────────────────────────────────
try:
    from flask import Flask, jsonify, redirect, render_template_string, request, send_file
except ImportError:
    sys.exit("Flask not found.  Install with:  pip install flask")

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
    from cryptography.x509.oid import NameOID
    import datetime as dt
except ImportError:
    sys.exit("cryptography not found.  Install with:  pip install cryptography")

# ── Paths ──────────────────────────────────────────────────────────────────
SCRIPT_DIR   = Path(__file__).resolve().parent
DATA_DIR     = SCRIPT_DIR / "ota_data"

SIGNING_KEY_PATH  = DATA_DIR / "signing_key.pem"
ECDSA_PUB_PATH    = DATA_DIR / "ecdsa_pub.pem"
SERVER_CERT_PATH  = DATA_DIR / "server_cert.pem"
SERVER_KEY_PATH   = DATA_DIR / "server_key.pem"
HMAC_SECRET_PATH  = DATA_DIR / "hmac_secret.txt"
STATE_PATH        = DATA_DIR / "state.json"

VARIANTS = ["fw_a", "fw_b"]

def _firmware_cert_dirs(project_root: Path) -> dict:
    return {
        "fw_a": project_root / "firmware_a" / "certs",
        "fw_b": project_root / "firmware_b" / "certs",
    }

# Default: project root is one level above the scripts/ directory.
# Override at runtime with --project-root when running from a different machine.
FIRMWARE_CERT_DIRS = _firmware_cert_dirs(SCRIPT_DIR.parent)


# ── Helpers ────────────────────────────────────────────────────────────────

def firmware_dir(variant: str) -> Path:
    d = DATA_DIR / variant
    d.mkdir(parents=True, exist_ok=True)
    return d


def binary_path(variant: str) -> Path:
    filename = _state.get(variant, {}).get("filename") or ""
    return firmware_dir(variant) / (filename or "sample_project.bin")


def manifest_path(variant: str) -> Path:
    return firmware_dir(variant) / "manifest.json"


def local_ip() -> str:
    """Best-effort: return the LAN IP of this machine."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"


# ── Key / cert generation ──────────────────────────────────────────────────

def generate_ecdsa_signing_keypair() -> None:
    """Generate ECDSA-P256 keypair for firmware signing."""
    print("  Generating ECDSA-P256 signing key …")
    key = ec.generate_private_key(ec.SECP256R1())

    SIGNING_KEY_PATH.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        )
    )
    SIGNING_KEY_PATH.chmod(0o600)

    ECDSA_PUB_PATH.write_bytes(
        key.public_key().public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo,
        )
    )
    print(f"    signing_key → {SIGNING_KEY_PATH}")
    print(f"    ecdsa_pub   → {ECDSA_PUB_PATH}")


def generate_tls_cert(host_ip: str) -> None:
    """Generate a self-signed TLS cert for the OTA server."""
    print(f"  Generating self-signed TLS cert (IP SAN={host_ip}) …")
    key = ec.generate_private_key(ec.SECP256R1())

    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, "esp32-ota-dev-server"),
    ])

    san_list = [x509.DNSName("localhost")]
    try:
        san_list.append(x509.IPAddress(ipaddress.ip_address(host_ip)))
    except ValueError:
        pass

    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(dt.datetime.now(dt.timezone.utc))
        .not_valid_after(dt.datetime.now(dt.timezone.utc) + dt.timedelta(days=3650))
        .add_extension(x509.SubjectAlternativeName(san_list), critical=False)
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )

    SERVER_KEY_PATH.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        )
    )
    SERVER_KEY_PATH.chmod(0o600)

    SERVER_CERT_PATH.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    print(f"    server_cert → {SERVER_CERT_PATH}")
    print(f"    server_key  → {SERVER_KEY_PATH}")


def run_setup(host_ip: str) -> None:
    """Generate all credentials and copy public material into firmware cert dirs."""
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    print("\n=== OTA Server Setup ===\n")

    if SIGNING_KEY_PATH.exists():
        print("  ECDSA signing key already exists — keeping it (re-run with --force-new-key to replace).")
    else:
        generate_ecdsa_signing_keypair()

    generate_tls_cert(host_ip)

    # Generate (or reuse) the shared HMAC secret in DATA_DIR — single source of truth.
    if HMAC_SECRET_PATH.exists():
        hmac_secret = HMAC_SECRET_PATH.read_text().strip()
        print("  HMAC secret already exists — reusing it.")
    else:
        hmac_secret = base64.urlsafe_b64encode(os.urandom(32)).decode()
        HMAC_SECRET_PATH.write_text(hmac_secret)
        print(f"  Generated HMAC secret → {HMAC_SECRET_PATH}")

    print("\n  Copying public credentials into firmware cert directories …")
    copy_ok = True
    for variant, cert_dir in FIRMWARE_CERT_DIRS.items():
        try:
            cert_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy(ECDSA_PUB_PATH,  cert_dir / "ecdsa_pub.pem")
            shutil.copy(SERVER_CERT_PATH, cert_dir / "server_ca.pem")
            # Placeholder MQTT + client certs — only written if absent
            for fname in ["mqtt_ca.pem", "client_cert.pem"]:
                dst = cert_dir / fname
                if not dst.exists():
                    shutil.copy(SERVER_CERT_PATH, dst)
            dst = cert_dir / "client_key.pem"
            if not dst.exists():
                shutil.copy(SERVER_KEY_PATH, dst)
            # Always write the HMAC secret so firmware certs stay in sync with DATA_DIR
            (cert_dir / "hmac_secret.txt").write_text(hmac_secret)
            print(f"    ✓ {cert_dir}")
        except OSError as e:
            print(f"    ✗ {cert_dir}  ({e})")
            copy_ok = False

    if not copy_ok:
        print(f"""
  Could not write to one or more firmware cert directories.
  If you are running this script from Windows, pass --project-root to point at
  the project on the WSL2 filesystem, e.g.:

    python scripts\\ota_server.py --setup --host <your-lan-ip> ^
        --project-root "\\\\wsl$\\Ubuntu\\home\\epk\\sample_project"

  Or copy the generated files manually from WSL2:

    cp {ECDSA_PUB_PATH}   firmware_a/certs/ecdsa_pub.pem
    cp {ECDSA_PUB_PATH}   firmware_b/certs/ecdsa_pub.pem
    cp {SERVER_CERT_PATH}  firmware_a/certs/server_ca.pem
    cp {SERVER_CERT_PATH}  firmware_b/certs/server_ca.pem
""")

    print("""
=== Next steps ===

1. Rebuild both firmware variants so the new certs are embedded:
     source "$HOME/.espressif/tools/activate_idf_v6.0.sh"
     ./build_and_flash.sh --build-only

2. Flash:
     ./build_and_flash.sh --flash-fw-a   # or --flash-fw-b / no args for both

3. Start the OTA server:
     python3 scripts/ota_server.py          # from WSL2
     python  scripts\\ota_server.py         # from Windows

4. Upload build artifacts to the server dashboard and trigger OTA.
""")


# ── Signing ────────────────────────────────────────────────────────────────

def load_signing_key() -> ec.EllipticCurvePrivateKey:
    if not SIGNING_KEY_PATH.exists():
        sys.exit(
            f"Signing key not found at {SIGNING_KEY_PATH}.\n"
            "Run:  python3 scripts/ota_server.py --setup"
        )
    return serialization.load_pem_private_key(
        SIGNING_KEY_PATH.read_bytes(), password=None
    )


def sign_binary(data: bytes, key: ec.EllipticCurvePrivateKey) -> str:
    """
    Sign binary with ECDSA-P256 / SHA-256.
    Returns base64-encoded raw r||s (64 bytes) as required by
    PSA psa_verify_hash(PSA_ALG_ECDSA(PSA_ALG_SHA_256)) on the device.
    """
    der_sig = key.sign(data, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der_sig)
    raw = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    return base64.b64encode(raw).decode()


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ── State persistence ──────────────────────────────────────────────────────

def _empty_variant_state() -> dict:
    return {
        "version":     "0.1.0",
        "filename":    "",
        "sha256":      "",
        "signature":   "",
        "uploaded_at": "",
        "binary_size": 0,
    }


def load_state() -> dict:
    if STATE_PATH.exists():
        try:
            return json.loads(STATE_PATH.read_text())
        except Exception:
            pass
    return {v: _empty_variant_state() for v in VARIANTS}


def save_state(state: dict) -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    STATE_PATH.write_text(json.dumps(state, indent=2))


def write_manifest(variant: str, state: dict, server_url: str) -> None:
    """
    Write manifest.json for a variant.

    Format matches parse_manifest() in ota_manager.cpp:
      { "firmware_a": { "version":..., "url":..., "sha256":..., "signature":... } }
    """
    fw_key = "firmware_a" if variant == "fw_a" else "firmware_b"
    manifest = {
        fw_key: {
            "version":   state[variant]["version"],
            "url":       f"{server_url}/{variant}/{state[variant].get('filename') or 'sample_project.bin'}",
            "sha256":    state[variant]["sha256"],
            "signature": state[variant]["signature"],
        }
    }
    manifest_path(variant).write_text(json.dumps(manifest, indent=2))


# ── HMAC helper (mirrors http_cmd.cpp verify_hmac logic) ──────────────────

def make_hmac_token(method: str, path: str, secret: str) -> str:
    """
    Produce the Authorization header value expected by the device HTTP server.
    Format:  HMAC-SHA256 <hex(HMAC-SHA256(secret, "<METHOD>\\n<PATH>\\n<unix_ts>"))>
    """
    ts = str(int(time.time()))
    msg = f"{method}\n{path}\n{ts}".encode()
    digest = hmac.new(secret.encode(), msg, hashlib.sha256).hexdigest()
    return f"HMAC-SHA256 {digest}", ts


# ── Flask app ──────────────────────────────────────────────────────────────

app = Flask(__name__)

_signing_key: ec.EllipticCurvePrivateKey | None = None
_server_url:  str = ""
_state:       dict = {}


# ── Static firmware + manifest routes ─────────────────────────────────────

@app.route("/<variant>/manifest.json")
def serve_manifest(variant: str):
    if variant not in VARIANTS:
        return jsonify(error="unknown variant"), 404
    p = manifest_path(variant)
    if not p.exists():
        return jsonify(error="manifest not yet generated — upload a binary first"), 404
    return send_file(p, mimetype="application/json")


@app.route("/<variant>/<filename>")
def serve_binary(variant: str, filename: str):
    if variant not in VARIANTS:
        return jsonify(error="unknown variant"), 404
    if not filename.endswith(".bin"):
        return jsonify(error="not found"), 404
    p = firmware_dir(variant) / filename
    if not p.exists():
        return jsonify(error="binary not uploaded"), 404
    return send_file(p, mimetype="application/octet-stream")


# ── Upload API ─────────────────────────────────────────────────────────────

@app.route("/api/upload/<variant>", methods=["POST"])
def api_upload(variant: str):
    """
    Accepts a multipart/form-data POST with:
      - file:    the .bin firmware image
      - version: (optional) semver string, e.g. "0.2.0"
    """
    global _state

    if variant not in VARIANTS:
        return jsonify(error="unknown variant"), 404

    if "file" not in request.files:
        return jsonify(error="no file field in request"), 400

    f = request.files["file"]
    data = f.read()
    if not data:
        return jsonify(error="empty file"), 400

    version = request.form.get("version", _state[variant]["version"]).strip()
    if not version:
        version = "0.1.0"

    # Compute SHA-256 and sign
    digest  = sha256_hex(data)
    sig_b64 = sign_binary(data, _signing_key)

    # Remove previous versioned binary if the filename will change
    new_filename = f"sample_project-v{version}.bin"
    old_filename = _state[variant].get("filename") or ""
    if old_filename and old_filename != new_filename:
        old_path = firmware_dir(variant) / old_filename
        old_path.unlink(missing_ok=True)

    # Persist binary under versioned filename
    (firmware_dir(variant) / new_filename).write_bytes(data)

    # Update state
    _state[variant].update({
        "version":     version,
        "filename":    new_filename,
        "sha256":      digest,
        "signature":   sig_b64,
        "uploaded_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "binary_size": len(data),
    })
    save_state(_state)
    write_manifest(variant, _state, _server_url)

    return jsonify(
        variant=variant,
        version=version,
        sha256=digest,
        size=len(data),
        manifest_url=f"{_server_url}/{variant}/manifest.json",
    )


# ── Version update API ─────────────────────────────────────────────────────

@app.route("/api/version/<variant>", methods=["POST"])
def api_set_version(variant: str):
    """
    Update the version string in the manifest without re-uploading the binary.
    Body (JSON or form): { "version": "0.3.0" }
    """
    global _state

    if variant not in VARIANTS:
        return jsonify(error="unknown variant"), 404

    if not binary_path(variant).exists():
        return jsonify(error="no binary uploaded yet for this variant"), 400

    data = request.get_json(silent=True) or request.form
    version = (data.get("version") or "").strip()
    if not version:
        return jsonify(error="version field required"), 400

    _state[variant]["version"] = version
    save_state(_state)
    write_manifest(variant, _state, _server_url)

    return jsonify(variant=variant, version=version)


# ── Device trigger API ─────────────────────────────────────────────────────

@app.route("/api/trigger", methods=["POST"])
def api_trigger():
    """
    Sends an OTA or slot-switch command to a running device.

    Device routes (http_cmd.cpp):
      POST /cmd/update?slot=A|B   — pull OTA from manifest and write to target slot
      POST /cmd/switch?fw=A|B     — switch active slot and reboot

    Body (JSON or form):
      device_ip:   IP of the device (e.g. "192.168.1.42")
      device_port: HTTP cmd server port (default: 8080)
      command:     "ota" or "switch"
      variant:     (for "ota")    "fw_a" or "fw_b" — which slot to update
      slot:        (for "switch") "firmware_a" or "firmware_b" — target slot
      hmac_secret: shared secret (read from firmware_a/certs/hmac_secret.txt if omitted)

    HMAC is computed over the full URI including query string, matching the device's
    verify_hmac() which uses req->uri (which includes the query string in ESP-IDF httpd).
    """
    import urllib.request

    data = request.get_json(silent=True) or request.form
    device_ip   = (data.get("device_ip")   or "").strip()
    device_port = int(data.get("device_port") or 8080)
    command     = (data.get("command")     or "ota").strip()
    variant     = (data.get("variant")     or "fw_a").strip()
    slot        = (data.get("slot")        or "").strip()
    secret      = (data.get("hmac_secret") or "").strip()

    if not device_ip:
        return jsonify(error="device_ip required"), 400

    # Load HMAC secret — prefer DATA_DIR copy (works from any machine),
    # fall back to firmware cert dirs for backwards compatibility.
    if not secret:
        if HMAC_SECRET_PATH.exists():
            secret = HMAC_SECRET_PATH.read_text().strip()
        else:
            secret_file = FIRMWARE_CERT_DIRS["fw_a"] / "hmac_secret.txt"
            if secret_file.exists():
                secret = secret_file.read_text().strip()
            else:
                return jsonify(
                    error=(
                        "hmac_secret not found. Run --setup to generate one, "
                        "or pass hmac_secret in the request body."
                    )
                ), 400

    if command == "ota":
        # /cmd/update?slot=A  (fw_a → A, fw_b → B)
        slot_char = "A" if variant == "fw_a" else "B"
        method = "POST"
        path   = f"/cmd/update?slot={slot_char}"
        body   = b""
    elif command == "switch":
        if not slot:
            return jsonify(error="slot required for switch command"), 400
        # /cmd/switch?fw=A|B  ("firmware_a" → A, "firmware_b" → B)
        fw_char = "A" if slot == "firmware_a" else "B"
        method = "POST"
        path   = f"/cmd/switch?fw={fw_char}"
        body   = b""
    else:
        return jsonify(error=f"unknown command '{command}'"), 400

    # HMAC must cover the full URI (path + query string) to match req->uri on device
    token, ts = make_hmac_token(method, path, secret)
    url = f"http://{device_ip}:{device_port}{path}"

    req = urllib.request.Request(
        url, data=body, method=method,
        headers={
            "Authorization": token,
            "X-Timestamp":   ts,
            "Content-Type":  "application/json",
        }
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            body_resp = resp.read().decode(errors="replace")
        return jsonify(status=resp.status, response=body_resp)
    except Exception as e:
        return jsonify(error=str(e)), 502


# ── Status API ─────────────────────────────────────────────────────────────

@app.route("/api/status")
def api_status():
    return jsonify(
        server_url=_server_url,
        variants={
            v: {
                **_state[v],
                "binary_present":   binary_path(v).exists(),
                "manifest_present": manifest_path(v).exists(),
                "manifest_url":     f"{_server_url}/{v}/manifest.json",
                "binary_url":       f"{_server_url}/{v}/{_state[v].get('filename') or 'sample_project.bin'}",
            }
            for v in VARIANTS
        }
    )


# ── Dashboard ──────────────────────────────────────────────────────────────

DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 OTA Dev Server</title>
<style>
  :root { --bg:#1a1a2e; --card:#16213e; --accent:#0f3460; --hi:#e94560;
          --text:#eaeaea; --muted:#888; --ok:#4caf50; --warn:#ff9800; }
  * { box-sizing: border-box; margin:0; padding:0; }
  body { background:var(--bg); color:var(--text); font:14px/1.6 'Segoe UI',sans-serif; padding:24px; }
  h1   { color:var(--hi); margin-bottom:4px; font-size:1.4rem; }
  .sub { color:var(--muted); font-size:.85rem; margin-bottom:24px; }
  .grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(400px,1fr)); gap:20px; }
  .card { background:var(--card); border-radius:8px; padding:20px; border:1px solid var(--accent); }
  .card h2 { font-size:1rem; margin-bottom:12px; color:var(--hi);
             display:flex; align-items:center; gap:8px; }
  .badge { padding:2px 8px; border-radius:12px; font-size:.75rem; font-weight:600; }
  .badge-ok   { background:var(--ok);   color:#000; }
  .badge-miss { background:var(--warn); color:#000; }
  table { width:100%; border-collapse:collapse; font-size:.85rem; }
  td { padding:5px 2px; border-bottom:1px solid var(--accent); }
  td:first-child { color:var(--muted); width:38%; }
  code { background:#0d0d1a; padding:1px 6px; border-radius:4px;
         font-family:monospace; word-break:break-all; font-size:.82rem; }
  label { display:block; margin-bottom:4px; color:var(--muted); font-size:.82rem; }
  input[type=text], input[type=number] {
    width:100%; padding:6px 10px; border-radius:4px;
    border:1px solid var(--accent); background:#0d0d1a; color:var(--text);
    font-size:.85rem; margin-bottom:10px; }
  .file-zone { border:2px dashed var(--accent); border-radius:4px; padding:10px 14px;
               cursor:pointer; margin-bottom:10px; transition:.15s; }
  .file-zone:hover { border-color:var(--hi); }
  .file-zone-hint { font-size:.82rem; color:var(--muted); }
  .file-zone-name { font-size:.82rem; color:var(--text); margin-top:3px; word-break:break-all; }
  .btn { display:inline-block; padding:7px 16px; border-radius:5px; border:none;
         cursor:pointer; font-size:.85rem; font-weight:600; transition:.15s; }
  .btn-primary { background:var(--hi); color:#fff; }
  .btn-primary:hover { opacity:.85; }
  .btn-secondary { background:var(--accent); color:var(--text); }
  .btn-secondary:hover { opacity:.85; }
  .row { display:flex; gap:8px; flex-wrap:wrap; }
  .msg { margin-top:10px; padding:8px 12px; border-radius:4px;
         font-size:.82rem; display:none; }
  .msg-ok  { background:#1b3d1f; color:var(--ok); }
  .msg-err { background:#3d1b1b; color:#f44336; }
  .section { margin-top:16px; padding-top:14px; border-top:1px solid var(--accent); }
  .section h3 { font-size:.9rem; color:var(--muted); margin-bottom:10px; }
  pre { background:#0d0d1a; padding:10px; border-radius:4px; font-size:.78rem;
        overflow-x:auto; white-space:pre-wrap; word-break:break-all; }
  .trigger-form input[type=text] { margin-bottom:6px; }
</style>
</head>
<body>
<h1>ESP32 OTA Dev Server</h1>
<p class="sub">Server: <code>{{ server_url }}</code></p>

<div class="grid" id="cards"></div>

<script>
const SERVER = "";   // relative — same origin

async function fetchStatus() {
  const r = await fetch(SERVER + "/api/status");
  return r.json();
}

function shortHash(h) { return h ? h.slice(0, 16) + "…" : "—"; }
function fmtSize(n) { return n ? (n/1024).toFixed(1) + " KB" : "—"; }

function renderCard(variant, info) {
  const label  = variant === "fw_a" ? "Firmware A" : "Firmware B";
  const badge  = info.binary_present
    ? `<span class="badge badge-ok">✓ binary</span>`
    : `<span class="badge badge-miss">no binary</span>`;

  return `
<div class="card" id="card-${variant}">
  <h2>${label} ${badge}</h2>
  <table>
    <tr><td>Version</td>  <td><code>${info.version || "—"}</code></td></tr>
    <tr><td>SHA-256</td>  <td><code>${shortHash(info.sha256)}</code></td></tr>
    <tr><td>Sig (r||s)</td><td><code>${info.signature ? info.signature.slice(0,20)+"…" : "—"}</code></td></tr>
    <tr><td>Size</td>     <td>${fmtSize(info.binary_size)}</td></tr>
    <tr><td>Uploaded</td> <td>${info.uploaded_at || "—"}</td></tr>
    <tr><td>Manifest</td> <td><a href="${info.manifest_url}" target="_blank" style="color:#e94560">${info.manifest_url}</a></td></tr>
  </table>

  <!-- Upload -->
  <div class="section">
    <h3>Upload binary</h3>
    <form id="upload-${variant}" onsubmit="doUpload(event,'${variant}')">
      <label>Firmware .bin</label>
      <input type="file" id="file-${variant}" name="file" accept=".bin"
             style="display:none" onchange="fileChanged(this,'${variant}')">
      <div class="file-zone" onclick="document.getElementById('file-${variant}').click()">
        <div class="file-zone-hint">Click to choose a .bin file</div>
        <div class="file-zone-name" id="file-name-${variant}">No file selected</div>
      </div>
      <label>Version</label>
      <input type="text" id="ver-upload-${variant}" name="version"
             value="${info.version || '0.1.0'}" required
             oninput="updateFileZoneDisplay('${variant}')">
      <button type="submit" class="btn btn-primary">Upload &amp; Sign</button>
    </form>
    <div id="upload-msg-${variant}" class="msg"></div>
  </div>

  <!-- Version only -->
  <div class="section">
    <h3>Update version (no re-upload)</h3>
    <form id="ver-${variant}" onsubmit="doVersion(event,'${variant}')">
      <div class="row">
        <input type="text" name="version" value="${info.version || '0.1.0'}"
               style="flex:1;margin-bottom:0" required>
        <button type="submit" class="btn btn-secondary">Set</button>
      </div>
    </form>
    <div id="ver-msg-${variant}" class="msg"></div>
  </div>

  <!-- Trigger -->
  <div class="section trigger-form">
    <h3>Trigger OTA on device</h3>
    <label>Device IP</label>
    <input type="text" id="ip-${variant}" placeholder="192.168.1.42">
    <label>Device HTTP port</label>
    <input type="number" id="port-${variant}" value="8080">
    <div class="row">
      <button class="btn btn-primary"  onclick="doTrigger('${variant}','ota')">Trigger OTA</button>
      <button class="btn btn-secondary" onclick="doSwitch('${variant}')">Switch Slot</button>
    </div>
    <div id="trig-msg-${variant}" class="msg"></div>
  </div>
</div>`;
}

async function doUpload(evt, variant) {
  evt.preventDefault();
  const form = evt.target;
  const fileInput = document.getElementById("file-" + variant);
  const msg = document.getElementById("upload-msg-" + variant);
  msg.style.display = "none";
  if (!fileInput.files || !fileInput.files.length) {
    showMsg(msg, "err", "Please select a .bin file first");
    return;
  }
  const fd = new FormData(form);
  try {
    const r = await fetch(`${SERVER}/api/upload/${variant}`, { method:"POST", body:fd });
    const j = await r.json();
    console.log("doUpload:", j);
    if (r.ok) {
      showMsg(msg, "ok", `✓ Uploaded v${j.version}  SHA-256: ${j.sha256.slice(0,16)}…`);
      setTimeout(refresh, 600);
    } else {
      showMsg(msg, "err", "Error: " + (j.error || JSON.stringify(j)));
    }
  } catch(e) { showMsg(msg, "err", e.message); }
}

async function doVersion(evt, variant) {
  evt.preventDefault();
  const version = evt.target.elements.version.value.trim();
  const msg = document.getElementById("ver-msg-" + variant);
  try {
    const r = await fetch(`${SERVER}/api/version/${variant}`, {
      method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify({version})
    });
    const j = await r.json();
    if (r.ok) { showMsg(msg, "ok", `✓ Version set to ${j.version}`); setTimeout(refresh, 400); }
    else       { showMsg(msg, "err", j.error || JSON.stringify(j)); }
  } catch(e) { showMsg(msg, "err", e.message); }
}

async function doTrigger(variant, command) {
  const msg  = document.getElementById("trig-msg-" + variant);
  const ip   = document.getElementById("ip-"   + variant).value.trim();
  const port = document.getElementById("port-" + variant).value.trim();
  if (!ip) { showMsg(msg, "err", "Enter device IP first"); return; }
  try {
    const r = await fetch(`${SERVER}/api/trigger`, {
      method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify({ device_ip:ip, device_port:port, command, variant })
    });
    const j = await r.json();
    console.log("doTriggerOTA:", j);
    if (r.ok) showMsg(msg, "ok", `✓ ${command} sent → /cmd/update (HTTP ${j.status})`);
    else      showMsg(msg, "err", j.error || JSON.stringify(j));
  } catch(e) { showMsg(msg, "err", e.message); }
}

async function doSwitch(variant) {
  // Switch TO the other slot (pressing fw_a card switches device to fw_b, and vice versa)
  const slot = variant === "fw_a" ? "firmware_b" : "firmware_a";
  const msg  = document.getElementById("trig-msg-" + variant);
  const ip   = document.getElementById("ip-"   + variant).value.trim();
  const port = document.getElementById("port-" + variant).value.trim();
  if (!ip) { showMsg(msg, "err", "Enter device IP first"); return; }
  try {
    const r = await fetch(`${SERVER}/api/trigger`, {
      method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify({ device_ip:ip, device_port:port, command:"switch", slot, variant })
    });
    const j = await r.json();
    console.log("doSwitchFirmware:", j);
    if (r.ok) showMsg(msg, "ok", `✓ Switch→${slot} sent → /cmd/switch (HTTP ${j.status})`);
    else      showMsg(msg, "err", j.error || JSON.stringify(j));
  } catch(e) { showMsg(msg, "err", e.message); }
}

function fileChanged(input, variant) {
  updateFileZoneDisplay(variant);
}

function updateFileZoneDisplay(variant) {
  const fileInput = document.getElementById("file-" + variant);
  const verInput  = document.getElementById("ver-upload-" + variant);
  const nameEl    = document.getElementById("file-name-" + variant);
  if (fileInput && fileInput.files && fileInput.files.length) {
    const fname   = fileInput.files[0].name;
    const version = verInput ? verInput.value.trim() : "";
    nameEl.textContent = version ? `${fname}  [v${version}]` : fname;
    nameEl.style.color = "var(--text)";
  } else {
    nameEl.textContent = "No file selected";
    nameEl.style.color = "var(--muted)";
  }
}

function showMsg(el, type, text) {
  el.className = "msg msg-" + (type === "ok" ? "ok" : "err");
  el.textContent = text;
  el.style.display = "block";
}

function updateCardStatus(variant, info) {
  const card = document.getElementById("card-" + variant);
  if (!card) return false;

  const label = variant === "fw_a" ? "Firmware A" : "Firmware B";
  const badge = info.binary_present
    ? `<span class="badge badge-ok">✓ binary</span>`
    : `<span class="badge badge-miss">no binary</span>`;
  card.querySelector("h2").innerHTML = `${label} ${badge}`;

  const cells = card.querySelectorAll("table td:nth-child(2)");
  cells[0].innerHTML = `<code>${info.version || "—"}</code>`;
  cells[1].innerHTML = `<code>${shortHash(info.sha256)}</code>`;
  cells[2].innerHTML = `<code>${info.signature ? info.signature.slice(0,20)+"…" : "—"}</code>`;
  cells[3].textContent = fmtSize(info.binary_size);
  cells[4].textContent = info.uploaded_at || "—";
  cells[5].innerHTML = `<a href="${info.manifest_url}" target="_blank" style="color:#e94560">${info.manifest_url}</a>`;

  return true;
}

async function refresh() {
  const status = await fetchStatus();
  const container = document.getElementById("cards");
  VARIANTS.forEach(v => {
    const info = status.variants[v];
    if (!updateCardStatus(v, info)) {
      container.insertAdjacentHTML("beforeend", renderCard(v, info));
    }
  });
}

const VARIANTS = ["fw_a", "fw_b"];
refresh();
setInterval(refresh, 15000);
</script>
</body>
</html>
"""


@app.route("/")
def dashboard():
    return render_template_string(DASHBOARD_HTML, server_url=_server_url)


@app.route("/favicon.ico")
def favicon():
    return "", 204


# ── Entry point ────────────────────────────────────────────────────────────

def main() -> None:
    global _signing_key, _server_url, _state

    parser = argparse.ArgumentParser(description="ESP32 local HTTPS OTA server")
    parser.add_argument("--host",  default="0.0.0.0",
                        help="Bind address for the server (default: 0.0.0.0). "
                             "In --setup mode, also used as the IP SAN in the TLS cert "
                             "if not 0.0.0.0 — otherwise the LAN IP is auto-detected.")
    parser.add_argument("--port",  default=8443, type=int, help="HTTPS port (default: 8443)")
    parser.add_argument("--setup", action="store_true",
                        help="Generate keys/certs and copy into firmware cert dirs, then exit")
    parser.add_argument("--force-new-key", action="store_true",
                        help="With --setup: replace the ECDSA signing key even if one exists "
                             "(all previously signed binaries must be re-uploaded afterward)")
    parser.add_argument("--project-root", default=None,
                        help="Path to the project root containing firmware_a/ and firmware_b/. "
                             "Useful when running from Windows to point at the WSL2 filesystem, e.g.: "
                             r'"\\wsl$\Ubuntu\home\epk\sample_project"')
    args = parser.parse_args()

    # Update firmware cert dirs if an explicit project root was given
    if args.project_root:
        global FIRMWARE_CERT_DIRS
        FIRMWARE_CERT_DIRS = _firmware_cert_dirs(Path(args.project_root))

    ip = local_ip() if args.host == "0.0.0.0" else args.host

    if args.setup:
        if args.force_new_key and SIGNING_KEY_PATH.exists():
            SIGNING_KEY_PATH.unlink()
            ECDSA_PUB_PATH.unlink()
        run_setup(ip)
        return

    # Verify credentials exist
    for path, hint in [
        (SIGNING_KEY_PATH, "--setup"),
        (SERVER_CERT_PATH, "--setup"),
        (SERVER_KEY_PATH,  "--setup"),
    ]:
        if not path.exists():
            sys.exit(f"{path} not found.  Run:  python3 scripts/ota_server.py --setup")

    _signing_key = load_signing_key()
    _state       = load_state()
    _server_url  = f"https://{ip}:{args.port}"

    # Regenerate manifests in case server URL changed (different IP)
    for v in VARIANTS:
        if binary_path(v).exists() and _state[v]["sha256"]:
            write_manifest(v, _state, _server_url)

    # Configure SSL
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(SERVER_CERT_PATH, SERVER_KEY_PATH)

    print(f"\n ESP32 OTA Dev Server")
    print(f" Dashboard : https://{ip}:{args.port}/")
    print(f" Manifests : https://{ip}:{args.port}/fw_a/manifest.json")
    print(f"             https://{ip}:{args.port}/fw_b/manifest.json")
    print(f" CA cert   : {SERVER_CERT_PATH}")
    print(f" Sign key  : {SIGNING_KEY_PATH}\n")

    app.run(host=args.host, port=args.port, ssl_context=ctx, debug=False)


if __name__ == "__main__":
    main()
