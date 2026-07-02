#ifndef AGENT_MEMORY_H
#define AGENT_MEMORY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

/* Agentic Memory Layer for CNET
 * - Own chat history (user / assistant turns)
 * - Internal thinking / thoughts (separate, for chain-of-thought style reasoning)
 * - Session persistence: remembers past sessions across runs via files
 *
 * Primary storage is now a single compact binary knowledge base:
 *   cnet_knowledge_base.bin  - stacks chat history, thoughts, and knowledge chunks.
 * This is much smaller than multiple raw .md text files and acts as the central KB.
 * Legacy separate .md files (if present from old runs) are auto-migrated once on first use.
 * No new .md memory files are written; use the bin (common-sense README and docs/ kept).
 */

#define AGENT_CHAT_FILE   "agent_chat_history.md"   /* legacy migration only; primary is cnet_knowledge_base.bin */
#define AGENT_THOUGHTS_FILE "agent_thoughts.md" /* legacy migration only; primary is cnet_knowledge_base.bin */

#define AGENT_ROLE_USER     "USER"
#define AGENT_ROLE_ASSISTANT "ASSISTANT"
#define AGENT_ROLE_THOUGHT  "THOUGHT"
#define AGENT_ROLE_SYSTEM   "SYSTEM"

/* Initialize / load last session from disk. Call once at agent start. */
int agent_memory_init(void);

/* Record a turn. role should be one of the AGENT_ROLE_* constants. */
int agent_record_turn(const char *role, const char *content);

/* Convenience wrappers */
int agent_record_user(const char *content);
int agent_record_assistant(const char *content);
int agent_record_thought(const char *content);   /* internal thinking */

/* Get recent chat history as a prompt-friendly string (last N turns).
 * Includes USER and ASSISTANT (optionally THOUGHT if include_thoughts). */
int agent_get_chat_context(char *out, size_t cap, int max_turns, int include_thoughts);

/* Get recent internal thoughts (for reflection / self-awareness). */
int agent_get_recent_thoughts(char *out, size_t cap, int max_thoughts);

/* Get a simple summary of what is currently loaded (for status/debug). */
int agent_get_session_summary(char *out, size_t cap);

/* Persist current history + thoughts to disk. */
int agent_save_session(void);

/* Clear in-memory history (does not delete files unless you want a reset). */
void agent_clear_session(void);

/* For advanced use: force reload from disk. */
int agent_reload(void);

/* Ingest a body of text (e.g. a book, chapter, or document) as knowledge.
 * It will be broken into chunks and recorded as THOUGHT entries tagged with the source.
 * This is the basic mechanism for "learning" a book at the agent memory layer.
 * Returns number of chunks recorded. */
int agent_ingest_text(const char *source_name, const char *text);

/* Retrieve knowledge snippets relevant to a query (very simple contains match for now).
 * Fills out with matching thoughts from ingested material. */
int agent_recall_knowledge(const char *query, char *out, size_t cap, int max_snippets);

/* Real file loading for books/documents. Reads the file at path and ingests as text with source_name.
 * Returns number of chunks or -1 on error. */
int agent_ingest_file(const char *source_name, const char *path);

/* Improved recall with entity awareness. */
int agent_recall_knowledge(const char *query, char *out, size_t cap, int max_snippets);

/* Build or rebuild the knowledge index from current thoughts (for 7B scalable retrieval). */
int agent_build_knowledge_index(void);

/* Get top-k relevant chunks using index (better than linear). Populates out with ranked snippets. */
int agent_recall_knowledge_indexed(const char *query, char *out, size_t cap, int max_snippets);

#define AGENT_KNOWLEDGE_INDEX_FILE "agent_knowledge_index.md"  /* legacy fallback read only; writes go to bin */

/* Consolidated Knowledge Base - single binary file for smaller size and easier management */
#define AGENT_KB_FILE "cnet_knowledge_base.bin"

/* Hierarchical memory (priority 2): summarize recent thoughts into higher level summary */
int agent_create_summary(char *summary_out, size_t cap, int num_recent);
int agent_get_hierarchical_context(char *out, size_t cap, int max_levels);

/* Shared utility: sanitize user input for safe filenames */
void sanitize_for_filename(const char *in, char *out, size_t cap);

/* HF dataset / remote text fetch support for training models from https://huggingface.co/datasets
 * Supports http(s) via curl.exe (Windows) or popen, and local files.
 * Returns FILE* for line-by-line read; caller must close with agent_close_text_source.
 */
FILE *agent_open_text_source(const char *source);
void agent_close_text_source(FILE *f, const char *source);

/* Simple JSON line extractor (for JSONL datasets like Fable traces).
 * Extracts "key": "value" string. Returns true if found and copied.
 */
bool agent_extract_json_string(const char *json_line, const char *key, char *out, size_t cap);

#endif /* AGENT_MEMORY_H */