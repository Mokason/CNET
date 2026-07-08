# Unlimited-OCR MCP

Processes PDF books with Unlimited-OCR model:
- OCR extraction via `convert_unlimited_ocr.py`
- Chunking + vectorization
- Local vector DB storage (JSONL + sqlite)
- Lightweight Obsidian surface overview notes

## Tools
- `process_pdf_book(pdf_path, book_title?)` → full pipeline + Obsidian note
- `get_book_overview(book_id)` → retrieve stored info

## Notes
- Currently uses placeholder embeddings (extend with sentence-transformers when torch env is ready)
- Obsidian notes are **surface level only** (high-level themes, not deep extraction)
- Integrates with CNET conversion script as requested

Created: 2026-07-06