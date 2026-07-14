#ifndef CCE_DETECT_H
#define CCE_DETECT_H

/* Universal pre-run model structure detection for CCE.
 *
 * Answers "what is this file and can we run it?" BEFORE any tensor data is
 * read. Detection is structural: the container format comes from magic bytes,
 * the architecture family comes from the tensor NAMES/SHAPES actually present
 * (declared arch strings like GGUF general.architecture are used as a label
 * and cross-check, never trusted alone).
 *
 * Probing cost: GGUF and safetensors loaders are already header-first
 * (metadata only), so cce_detect_file on a 12B-parameter file reads a few KB.
 *
 * Two layers:
 *   cce_detect_file()   - fill a cce_model_info report, never loads weights.
 *   cce_anymodel_open() - detect, then dispatch to the matching CCE loader.
 *
 * Unsupported structures are reported honestly (runnable=0 + notes) instead
 * of being mis-run through the wrong forward pass.
 */

#include "cce_defs.h"
#include "cce_gguf.h"
#include "cce_safetensors.h"
#include "cce_forest.h"
#include "cce_model.h"
#include "cce_ssm.h"
#include "cce_hybrid.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CCE_FMT_UNKNOWN = 0,
    CCE_FMT_GGUF,          /* "GGUF" magic */
    CCE_FMT_SAFETENSORS,   /* 8-byte LE header length + '{' JSON header */
    CCE_FMT_CCE_ARCHIVE,   /* "CCE1" forest archive */
    CCE_FMT_CMDL,          /* "CMDL" cce_model save file */
    CCE_FMT_SUPRA_PACK,    /* 'SUPK' packed ternary supra */
    CCE_FMT_QWEN2_PACK,    /* 'QGKP' packed ternary gguf-transformer */
} cce_container_format;

typedef enum {
    CCE_ARCH_FAMILY_UNKNOWN = 0,
    CCE_ARCH_FAMILY_LLAMA,   /* separate q/k/v/o attention + gated MLP (llama/qwen/mistral/gemma...) */
    CCE_ARCH_FAMILY_GPT2,    /* fused qkv attention (gpt2 c_attn / supra blocks.N.attn.qkv) */
    CCE_ARCH_FAMILY_MAMBA,   /* state-space mixer (ssm_* / mixer.A_log) */
    CCE_ARCH_FAMILY_HYBRID,  /* interleaved attention + state-space (jamba/zamba) */
    CCE_ARCH_FAMILY_MLP,     /* plain layers.N linear stack */
    CCE_ARCH_FAMILY_CCE,     /* CCE-native (forest archive / cce_model) */
} cce_arch_family;

typedef struct {
    cce_container_format format;
    cce_arch_family family;
    char arch[64];           /* declared arch string when the file carries one ("" if none) */
    char naming[32];         /* tensor naming scheme fingerprint, e.g. "hf-model.layers" */
    char dtype[16];          /* dominant tensor dtype, e.g. "F32", "Q8_0" */
    int  n_tensors;          /* -1 unknown */
    int  n_layer;            /* -1 unknown */
    int  hidden;             /* embedding width, -1 unknown */
    int  n_head;             /* -1 unknown */
    int  n_kv_head;          /* -1 unknown */
    int  vocab;              /* -1 unknown */
    int  ffn;                /* feed-forward width, -1 unknown */
    int  ctx_len;            /* -1 unknown */
    int  tied_embeddings;    /* 1 = lm head shares token embedding, 0 = separate, -1 unknown */
    int  attention_full_qkv; /* 1 = q,k,v,o all present; 0 = partial (e.g. q+o only); -1 n/a */
    int  is_moe;             /* 1 = expert/router tensors present; 0 = no structural MoE evidence */
    int  runnable;           /* 1 = an existing CCE runner handles this structure */
    char runner[64];         /* recommended entry point when runnable */
    char notes[256];         /* mismatches, caveats, what to do when not runnable */
} cce_model_info;

/* Probe a model file. Reads metadata only (no tensor data).
 * Returns CCE_OK when the file was probed (even if format==UNKNOWN);
 * IO errors return CCE_ERR_IO / CCE_ERR_INVALID_ARG.
 */
cce_result cce_detect_file(const char* path, cce_model_info* out);

const char* cce_detect_format_name(cce_container_format f);
const char* cce_detect_family_name(cce_arch_family f);

/* Human-readable one-screen report to stdout. */
void cce_detect_print(const cce_model_info* info, const char* path);

/* ---- Universal open: detect, then dispatch via the runner registry ----
 *
 * The registry (cce_detect.c) is the single arch->builder table: it decides
 * both the "runnable/runner" verdict in cce_detect_file and the actual
 * dispatch in cce_anymodel_open. Adding a runner = adding one table row. */

typedef struct {
    cce_model_info info;
    /* exactly one of these is non-NULL, chosen by the registry */
    cce_gguf_qwen2* transformer;  /* GGUF llama-family, HF-llama safetensors, 'QGKP' packed */
    cce_supra_a2a*  supra;        /* 'SUPK' packed + supra-style safetensors */
    cce_ssm_model*  ssm;          /* mamba-family (safetensors or GGUF) */
    cce_hybrid_model* hybrid;     /* interleaved attention+ssm (jamba-style GGUF) */
    cce_forest*     forest;       /* "CCE1" archive */
    cce_model*      model;        /* "CMDL" cce_model save */
} cce_anymodel;

/* Detect and load. CCE_ERR_UNSUPPORTED when the structure has no runner yet
 * (out->info in that case is not available; call cce_detect_file for the report). */
cce_result cce_anymodel_open(cce_anymodel** out, const char* path);
void cce_anymodel_free(cce_anymodel* m);

#ifdef __cplusplus
}
#endif

#endif /* CCE_DETECT_H */
