/* GGUF-embedded GPT-2 / Qwen BPE tokenizer (tokens + merges).
 *
 * Loads tokenizer.ggml.tokens / .merges / .token_type from a GGUF without
 * materializing model tensors. Encode/decode use the GPT-2 byte↔unicode
 * map (Ġ/Ċ …) so Qwen3.5 / Qwythos pieces round-trip correctly.
 *
 * Chat helper builds the text-only Qwythos / Qwen ChatML template:
 *   <|im_start|>system … <|im_end|>
 *   <|im_start|>user … <|im_end|>
 *   <|im_start|>assistant\n<think>\n   (optional empty-think)
 */
#ifndef CCE_GGUF_TOK_H
#define CCE_GGUF_TOK_H

#include "cce_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_gguf_tok cce_gguf_tok;

/* Parse tokenizer tables from path.gguf (KV only). */
cce_result cce_gguf_tok_load(const char *gguf_path, cce_gguf_tok **out);
void cce_gguf_tok_free(cce_gguf_tok *t);

int cce_gguf_tok_vocab_size(const cce_gguf_tok *t);
int cce_gguf_tok_eos_id(const cce_gguf_tok *t);   /* <|im_end|> if known, else pad/eos */
int cce_gguf_tok_bos_id(const cce_gguf_tok *t);   /* often -1 for Qwen */
int cce_gguf_tok_is_special(const cce_gguf_tok *t, int id);
const char *cce_gguf_tok_piece(const cce_gguf_tok *t, int id); /* raw piece or NULL */

/* Encode UTF-8 text. Literals matching special pieces are emitted as single ids.
 * Returns number of ids written (0 on failure / empty). */
int cce_gguf_tok_encode(const cce_gguf_tok *t, const char *text,
                        int *ids, int max_ids);

/* Decode to UTF-8. skip_special drops CONTROL/USER_DEFINED tokens.
 * Returns bytes written excluding NUL; out always NUL-terminated if max_out>0. */
int cce_gguf_tok_decode(const cce_gguf_tok *t, const int *ids, int n_ids,
                        char *out, int max_out, int skip_special);

/* Text-only Qwythos/Qwen chat template → out string.
 * system NULL/empty → default Qwythos identity blurb only.
 * enable_thinking: 1 → open <think>\n ; 0 → empty think then answer.
 * Returns length written (excl NUL) or -1 on overflow. */
int cce_gguf_tok_chat_template(char *out, int max_out,
                               const char *system, const char *user,
                               int enable_thinking);

/* Template + encode. Returns id count. */
int cce_gguf_tok_encode_chat(const cce_gguf_tok *t,
                             const char *system, const char *user,
                             int enable_thinking,
                             int *ids, int max_ids);

#ifdef __cplusplus
}
#endif
#endif /* CCE_GGUF_TOK_H */
