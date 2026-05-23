#!/usr/bin/env python3
"""Generate src/webJqueryMinJs.cpp from jquery.min.js (PROGMEM)."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_JS = ROOT / "UploadFiles" / "jquery.min.js"
SNM_JS = Path(r"C:\DevWork2\Esp32SNMPforSUN\UploadFiles\jquery.min.js")
OUT_CPP = ROOT / "src" / "webJqueryMinJs.cpp"

js_path = SNM_JS if SNM_JS.is_file() else SRC_JS
js = js_path.read_text(encoding="utf-8")
escaped = (
    js.replace("\\", "\\\\")
    .replace('"', '\\"')
    .replace("\n", "\\n")
    .replace("\r", "")
)
chunk_size = 6000
parts = [escaped[i : i + chunk_size] for i in range(0, len(escaped), chunk_size)]

lines = [
    '#include "webJqueryMinJs.h"',
    "",
    "const char jquery_min_js[] PROGMEM =",
]
for i, part in enumerate(parts):
    lines.append('  "' + part + '"' + (";" if i == len(parts) - 1 else ""))
lines.append("")

OUT_CPP.write_text("\n".join(lines), encoding="utf-8")
print(f"Wrote {OUT_CPP} ({len(js)} bytes JS, {len(parts)} chunks)")
