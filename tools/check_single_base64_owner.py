#!/usr/bin/env python3
"""Production SaltsNet may only consume <base64_utils.h> / tn_base64_*.

Source ownership is in SaltsCore::Core, never another codec in MIME, SMTP or
a vendored libbase64/aklomp. MIME line folding is presentation, not an encoder.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MIME = ROOT / "email/mime_parser"
assert not (MIME / "include/mime_base64.h").exists(), "duplicate public MIME codec"
assert not (MIME / "src/mime_base64.c").exists(), "duplicate native MIME codec"

required = [
    "email/mime_parser/src/mime_utils.c",
    "email/mime_parser/src/mime_encoded_word.c",
    "email/mime_parser/src/mime_mhtml.c",
    "email/src/email_message.c",
    "email/src/email_smtp.c",
    "tproxy/src/salts_tcp_proxy.c",
]
for name in required:
    content = (ROOT / name).read_text(encoding="utf-8")
    if "#include <base64_utils.h>" not in content:
        raise AssertionError(f"{name}: missing canonical SaltsCore Base64 import")
    if "mime_base64_encode" in content or "mime_base64_decode" in content or \
       "mime_base64_encoded_size" in content:
        raise AssertionError(f"{name}: duplicate native Base64 codec call")
    if not re.search(r"\btn_base64_[a-z_]+\(", content):
        raise AssertionError(f"{name}: no canonical tn_base64_* call")

for folder in ("email", "tproxy", "ice", "snmp", "ldap", "lb"):
    for path in (ROOT / folder).rglob("*"):
        if path.suffix not in (".c", ".h", ".cpp"):
            continue
        content = path.read_text(encoding="utf-8", errors="replace")
        if re.search(r"(?m)^#\s*include\s*[<\"](?:mime_base64\.h|libbase64\.h)[>\"]", content):
            raise AssertionError(f"{path.relative_to(ROOT)}: private duplicate codec include")
        if re.search(r"\bstatic\s+(?:inline\s+)?(?:char\s*\*|int)\s*"
                     r"(?:base64_encode|base64_decode)\s*\(", content):
            raise AssertionError(f"{path.relative_to(ROOT)}: local Base64 codec implementation")
print("PASS: SaltsNet consumes sole SaltsCore tn_base64_* ABI, no other codec owner")
