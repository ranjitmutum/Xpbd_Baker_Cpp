#!/usr/bin/env python3
"""One-shot helper: dump hardcoded tables from i18n.cpp into i18n/*.json (if present)."""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "app" / "i18n.cpp"
OUT = ROOT / "i18n"


def extract(fn_name: str, src: str) -> dict[str, str]:
    m = re.search(
        rf"const Table& {fn_name}\(\) \{{.*?static const Table t = \{{(.*?)\n    \}};",
        src,
        re.S,
    )
    if not m:
        raise SystemExit(f"cannot find {fn_name}")
    body = m.group(1)
    pairs: dict[str, str] = {}
    for mm in re.finditer(r'\{\s*"([^"]+)"\s*,\s*((?:"[^"]*"\s*)+)\}', body):
        key = mm.group(1)
        parts = re.findall(r'"([^"]*)"', mm.group(2))
        pairs[key] = "".join(parts)
    return pairs


def main() -> None:
    src = SRC.read_text(encoding="utf-8")
    OUT.mkdir(exist_ok=True)
    mapping = {
        "tableEn": "en.json",
        "tableZhCn": "zh-CN.json",
        "tableZhHk": "zh-HK.json",
        "tableZhTw": "zh-TW.json",
    }
    for fn, name in mapping.items():
        if f"{fn}()" not in src:
            print("skip", fn)
            continue
        d = extract(fn, src)
        (OUT / name).write_text(
            json.dumps(d, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        print(name, len(d), "keys")


if __name__ == "__main__":
    main()
