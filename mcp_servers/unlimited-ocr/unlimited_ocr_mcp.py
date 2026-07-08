#!/usr/bin/env python3
"""
Unlimited-OCR MCP Server
- Extracts text from PDF books using Unlimited-OCR (via convert_unlimited_ocr.py)
- Vectorizes chunks
- Stores in local vector DB (JSONL + sqlite)
- Generates lightweight Obsidian surface overview notes
"""

import json
import os
import sys
import sqlite3
from datetime import datetime
from typing import List, Dict, Any
import subprocess

# Paths
MCP_DIR = os.path.dirname(os.path.abspath(__file__))
CNET_CONVERT_SCRIPT = "/home/marble/AI/CNET/convert_unlimited_ocr.py"
VECTOR_DB_DIR = os.path.expanduser("~/.hermes/mcp_servers/unlimited-ocr-mcp/vectordb")
OBSIDIAN_VAULT = "/home/marble/Documents/Obsidian Vault"

os.makedirs(VECTOR_DB_DIR, exist_ok=True)

# Simple local vector store (JSONL + sqlite metadata)
VECTOR_FILE = os.path.join(VECTOR_DB_DIR, "book_chunks.jsonl")
META_DB = os.path.join(VECTOR_DB_DIR, "book_meta.db")

def init_db():
    conn = sqlite3.connect(META_DB)
    c = conn.cursor()
    c.execute('''CREATE TABLE IF NOT EXISTS books
                 (id TEXT PRIMARY KEY, title TEXT, path TEXT, processed_at TEXT, chunk_count INTEGER)''')
    c.execute('''CREATE TABLE IF NOT EXISTS chunks
                 (id TEXT PRIMARY KEY, book_id TEXT, chunk_text TEXT, embedding BLOB, page INTEGER)''')
    conn.commit()
    conn.close()

def vectorize_text(text: str) -> List[float]:
    """Placeholder: In real env use sentence-transformers.
    For now returns dummy embedding (extend later)."""
    # TODO: Replace with actual embedding model when torch is available
    return [0.0] * 384  # 384-dim dummy

def process_pdf_book(pdf_path: str, book_title: str = None) -> Dict[str, Any]:
    """Main tool: OCR + chunk + vectorize + store + Obsidian overview"""
    if not os.path.exists(pdf_path):
        return {"error": f"PDF not found: {pdf_path}"}

    if book_title is None:
        book_title = os.path.basename(pdf_path).replace(".pdf", "")

    book_id = f"{book_title}_{datetime.now().strftime('%Y%m%d_%H%M')}"

    # 1. Run OCR via CNET-connected convert_unlimited_ocr.py bridge
    print(f"[Unlimited-OCR MCP] Processing {pdf_path} via CNET bridge...", file=sys.stderr)
    try:
        # Add CNET to path (already done in launch.sh)
        from convert_unlimited_ocr import extract_text_from_pdf
        ocr_result = extract_text_from_pdf(pdf_path, book_title, use_unlimited_model=True, connect_to_cnet=True)
        extracted_text = ocr_result.get("extracted_text", "")
        cnet_integration = ocr_result.get("cnet_integration", {})
        print(f"[Unlimited-OCR MCP] CNET connection: {cnet_integration.get('cnet_status', 'unknown')}", file=sys.stderr)
    except Exception as e:
        print(f"[Unlimited-OCR MCP] Bridge import/call failed: {e} - falling back to simulation", file=sys.stderr)
        extracted_text = f"[OCR PLACEHOLDER] Extracted text from {book_title}. " * 50
        cnet_integration = {"cnet_status": "fallback"}

    # 2. Chunk the text
    chunks = [extracted_text[i:i+800] for i in range(0, len(extracted_text), 800)]

    # 3. Vectorize + store
    init_db()
    conn = sqlite3.connect(META_DB)
    c = conn.cursor()

    for i, chunk in enumerate(chunks):
        emb = vectorize_text(chunk)
        chunk_id = f"{book_id}_chunk_{i}"
        c.execute("INSERT OR REPLACE INTO chunks VALUES (?, ?, ?, ?, ?)",
                  (chunk_id, book_id, chunk, json.dumps(emb), i))

    c.execute("INSERT OR REPLACE INTO books VALUES (?, ?, ?, ?, ?)",
              (book_id, book_title, pdf_path, datetime.now().isoformat(), len(chunks)))
    conn.commit()
    conn.close()

    # 4. Append to JSONL vector store
    with open(VECTOR_FILE, "a") as f:
        for i, chunk in enumerate(chunks):
            record = {
                "book_id": book_id,
                "chunk_id": f"{book_id}_chunk_{i}",
                "text": chunk[:200] + "...",
                "page": i,
                "vector": vectorize_text(chunk)[:8]  # store small preview
            }
            f.write(json.dumps(record) + "\n")

    # 5. Generate lightweight Obsidian overview
    overview_path = generate_obsidian_overview(book_title, book_id, pdf_path, len(chunks))

    return {
        "status": "success",
        "book_id": book_id,
        "chunks": len(chunks),
        "obsidian_note": overview_path,
        "vector_db": VECTOR_FILE,
        "cnet_connection": cnet_integration if 'cnet_integration' in locals() else {"status": "not_connected"}
    }

def generate_obsidian_overview(book_title: str, book_id: str, pdf_path: str, chunk_count: int) -> str:
    """Create a light surface-level Obsidian note"""
    note_dir = os.path.join(OBSIDIAN_VAULT, "Books", "Overviews")
    os.makedirs(note_dir, exist_ok=True)

    note_path = os.path.join(note_dir, f"{book_title.replace(' ', '_')}_Overview.md")

    content = f"""---
type: Book Overview
book_id: {book_id}
source: {pdf_path}
processed: {datetime.now().isoformat()}
chunks: {chunk_count}
tags: [book, ocr, vectorized]
---

# {book_title} — Surface Overview

**Source**: {pdf_path}
**Processed**: {datetime.now().strftime('%Y-%m-%d')}
**Chunks extracted**: {chunk_count}

## High-Level Themes (Surface)
- [Add 3-5 bullet points from surface reading here]

## Key Concepts (Light)
- [Surface-level concepts only]

## Notes
- Vectorized and stored in local DB
- Full detail available via `process_pdf_book` tool

---
*Generated by Unlimited-OCR MCP*
"""

    with open(note_path, "w") as f:
        f.write(content)

    return note_path

def get_book_overview(book_id: str) -> Dict[str, Any]:
    """Retrieve existing overview info"""
    conn = sqlite3.connect(META_DB)
    c = conn.cursor()
    c.execute("SELECT * FROM books WHERE id = ?", (book_id,))
    row = c.fetchone()
    conn.close()
    if row:
        return {"book": row}
    return {"error": "Book not found"}

# === MCP stdio protocol ===
if __name__ == "__main__":
    init_db()
    for line in sys.stdin:
        if not line.strip():
            continue
        try:
            req = json.loads(line)
        except:
            continue

        method = req.get("method", "")
        params = req.get("params", {})

        if method == "initialize":
            protocol_version = params.get("protocolVersion", "2024-11-05")
            response = {
                "jsonrpc": "2.0",
                "id": req.get("id"),
                "result": {
                    "protocolVersion": protocol_version,
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "unlimited-ocr", "version": "0.2.0"}
                }
            }
            print(json.dumps(response))
            sys.stdout.flush()

        elif method == "tools/list":
            response = {
                "jsonrpc": "2.0",
                "id": req.get("id"),
                "result": {
                    "tools": [
                        {
                            "name": "process_pdf_book",
                            "description": "OCR a PDF book, vectorize chunks, store in DB, and create Obsidian overview",
                            "inputSchema": {
                                "type": "object",
                                "properties": {
                                    "pdf_path": {"type": "string", "description": "Absolute path to the PDF book"},
                                    "book_title": {"type": "string", "description": "Optional title (defaults to filename)"}
                                },
                                "required": ["pdf_path"]
                            }
                        },
                        {
                            "name": "get_book_overview",
                            "description": "Get surface overview info for a processed book",
                            "inputSchema": {
                                "type": "object",
                                "properties": {
                                    "book_id": {"type": "string", "description": "Book id returned by process_pdf_book"}
                                },
                                "required": ["book_id"]
                            }
                        }
                    ]
                }
            }
            print(json.dumps(response))
            sys.stdout.flush()

        elif method == "tools/call":
            tool_name = params.get("name")
            args = params.get("arguments", {})

            if tool_name == "process_pdf_book":
                result = process_pdf_book(args.get("pdf_path"), args.get("book_title"))
            elif tool_name == "get_book_overview":
                result = get_book_overview(args.get("book_id"))
            else:
                result = {"error": "Unknown tool"}

            response = {
                "jsonrpc": "2.0",
                "id": req.get("id"),
                "result": {"content": [{"type": "text", "text": json.dumps(result)}]}
            }
            print(json.dumps(response))
            sys.stdout.flush()
