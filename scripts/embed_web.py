#!/usr/bin/env python3
"""Comprime los recursos web y genera include/web_assets.h de forma determinista."""

from __future__ import annotations

import gzip
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "include" / "web_assets.h"
ASSETS = (
    ("INDEX_HTML", ROOT / "web" / "index.html"),
    ("STYLES_CSS", ROOT / "web" / "styles.css"),
    ("APP_JS", ROOT / "web" / "app.js"),
)


def format_bytes(data: bytes) -> str:
    rows = []
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        rows.append("    " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    return "\n".join(rows)


def generate() -> None:
    sections = [
        "// Archivo generado por scripts/embed_web.py. No editar manualmente.",
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        "namespace WebAssets {",
        "",
    ]

    for symbol, path in ASSETS:
        source = path.read_bytes()
        compressed = gzip.compress(source, compresslevel=9, mtime=0)
        sections.extend(
            [
                f"static const uint8_t {symbol}_GZ[] PROGMEM = {{",
                format_bytes(compressed),
                "};",
                f"static constexpr size_t {symbol}_GZ_LEN = sizeof({symbol}_GZ);",
                "",
            ]
        )

    sections.extend(["}  // namespace WebAssets", ""])
    content = "\n".join(sections)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)

    if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != content:
        OUTPUT.write_text(content, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    generate()
