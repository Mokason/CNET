#!/usr/bin/env python3
"""Extract spreadsheet → TSV on stdout for ROE table router.
Supports: .xlsx (stdlib zip+xml), .xls via libreoffice→csv if available,
          .csv/.tsv passthrough.
"""
from __future__ import annotations

import csv
import io
import os
import re
import sys
import tempfile
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path


NS = {
    "m": "http://schemas.openxmlformats.org/spreadsheetml/2006/main",
    "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships",
}


def col_row(cell_ref: str):
    m = re.match(r"([A-Z]+)(\d+)", cell_ref or "")
    if not m:
        return 0, 0
    col = 0
    for ch in m.group(1):
        col = col * 26 + (ord(ch) - 64)
    return col - 1, int(m.group(2)) - 1


def shared_strings(z: zipfile.ZipFile):
    try:
        root = ET.fromstring(z.read("xl/sharedStrings.xml"))
    except KeyError:
        return []
    out = []
    for si in root.findall("m:si", NS):
        texts = [t.text or "" for t in si.findall(".//m:t", NS)]
        out.append("".join(texts))
    return out


def xlsx_to_rows(path: str):
    with zipfile.ZipFile(path, "r") as z:
        ss = shared_strings(z)
        # first worksheet
        sheet_path = None
        for n in z.namelist():
            if n.startswith("xl/worksheets/sheet") and n.endswith(".xml"):
                sheet_path = n
                break
        if not sheet_path:
            return []
        root = ET.fromstring(z.read(sheet_path))
        grid = {}
        max_r = max_c = 0
        for c in root.findall(".//m:c", NS):
            ref = c.get("r")
            if not ref:
                continue
            ci, ri = col_row(ref)
            max_r = max(max_r, ri)
            max_c = max(max_c, ci)
            t = c.get("t")
            v = c.find("m:v", NS)
            val = ""
            if v is not None and v.text is not None:
                if t == "s":
                    try:
                        val = ss[int(v.text)]
                    except Exception:
                        val = v.text
                else:
                    val = v.text
            grid[(ri, ci)] = val
        rows = []
        for r in range(max_r + 1):
            row = [grid.get((r, c), "") for c in range(max_c + 1)]
            if any(str(x).strip() for x in row):
                rows.append(row)
        return rows


def xls_via_libreoffice(path: str):
    outdir = tempfile.mkdtemp(prefix="roe_xls_")
    cmd = (
        f"libreoffice --headless --convert-to csv --outdir {outdir} "
        f"'{path}' >/dev/null 2>&1"
    )
    os.system(cmd)
    csvs = list(Path(outdir).glob("*.csv"))
    if not csvs:
        return []
    with open(csvs[0], newline="", encoding="utf-8", errors="replace") as f:
        return [list(r) for r in csv.reader(f)]


def main():
    if len(sys.argv) < 2:
        print("usage: roe_table_extract.py PATH", file=sys.stderr)
        return 2
    path = sys.argv[1]
    ext = Path(path).suffix.lower()
    rows = []
    if ext in {".csv", ".tsv"}:
        delim = "\t" if ext == ".tsv" else ","
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            rows = [list(r) for r in csv.reader(f, delimiter=delim)]
    elif ext == ".xlsx":
        rows = xlsx_to_rows(path)
    elif ext == ".xls":
        rows = xls_via_libreoffice(path)
    else:
        print(f"unsupported {ext}", file=sys.stderr)
        return 2

    w = csv.writer(sys.stdout, delimiter="\t", lineterminator="\n")
    for row in rows:
        w.writerow([str(c).replace("\t", " ").replace("\n", " ") for c in row])
    return 0


if __name__ == "__main__":
    sys.exit(main())
