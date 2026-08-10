#include "../include/cnet_roe_self.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Optional agent reflection — weak link style: call if symbols resolve via
 * direct include; keep build hermetic (agent_memory is stdlib-only). */
#include "../include/agent_memory.h"

static int mkdir_p(const char *path) {
    char tmp[ROE_SELF_PATH];
    size_t len, i;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return mkdir(tmp, 0755);
}

static int file_contains(const char *path, const char *needle) {
    FILE *f;
    char buf[4096];
    size_t n;
    int hit = 0;
    if (!path || !needle || !needle[0]) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while ((n = fread(buf, 1, sizeof buf - 1, f)) > 0) {
        buf[n] = 0;
        if (strstr(buf, needle)) {
            hit = 1;
            break;
        }
    }
    fclose(f);
    return hit;
}

static int file_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

#define ROE_SELF_PATH_LOCAL 1024

static void join_root(const RoeSelfModel *M, char *out, size_t cap, const char *rel) {
    if (!out || !cap) return;
    out[0] = 0;
    if (M && M->repo_root[0]) {
        size_t n = strlen(M->repo_root);
        size_t r = rel ? strlen(rel) : 0;
        if (n + 1 + r + 1 > cap) {
            /* fail-closed short path: just use rel */
            if (rel) {
                size_t k = r < cap - 1 ? r : cap - 1;
                memcpy(out, rel, k);
                out[k] = 0;
            }
            return;
        }
        memcpy(out, M->repo_root, n);
        out[n] = '/';
        if (rel) memcpy(out + n + 1, rel, r);
        out[n + 1 + r] = 0;
    } else if (rel) {
        size_t r = strlen(rel);
        size_t k = r < cap - 1 ? r : cap - 1;
        memcpy(out, rel, k);
        out[k] = 0;
    }
}

static int path_join2(char *out, size_t cap, const char *a, const char *b) {
    size_t na, nb;
    if (!out || !cap || !a || !b) return -1;
    na = strlen(a);
    nb = strlen(b);
    if (na + 1 + nb + 1 > cap) return -1;
    memcpy(out, a, na);
    out[na] = '/';
    memcpy(out + na + 1, b, nb);
    out[na + 1 + nb] = 0;
    return 0;
}

static void set_rank_ev(RoeTree *T, const char *id, int rank, const char *ev,
                        RoeTreeEvidenceRow *rows, size_t *n_rows, size_t cap) {
    size_t i;
    if (!T || !id) return;
    for (i = 0; i < T->n_nodes; i++) {
        if (!T->nodes[i].active || strcmp(T->nodes[i].id, id) != 0) continue;
        if (rank > T->nodes[i].max_rank) rank = T->nodes[i].max_rank;
        if (rank < 0) rank = 0;
        /* Never lower a rank already higher from stronger evidence in-session. */
        if (rank > T->nodes[i].rank) T->nodes[i].rank = rank;
        if (rows && n_rows && *n_rows < cap) {
            RoeTreeEvidenceRow *r = &rows[*n_rows];
            memset(r, 0, sizeof *r);
            snprintf(r->node_id, sizeof r->node_id, "%s", id);
            r->rank = T->nodes[i].rank;
            r->max_rank = T->nodes[i].max_rank;
            snprintf(r->evidence, sizeof r->evidence, "%s", ev ? ev : "none");
            (*n_rows)++;
        }
        return;
    }
}

/* Built-in OCR surpass tree (shared with skill_tree CLI semantics). */
static int seed_ocr_tree(RoeTree *T) {
    if (!T) return -1;
    roe_tree_init(T);
    roe_tree_add_node(T, "root_doc", "Document OCR Domain", "ocr",
                      "Page perception root", 3, 1.0, 1.0);
    roe_tree_add_node(T, "root_shell", "Shell / ASI Law", "shell",
                      "Verify, abstain, never self-CERT", 3, 1.0, 1.1);
    roe_tree_add_node(T, "root_sys", "System Product", "meta",
                      "Packs + hybrid product", 3, 1.0, 1.0);

    roe_tree_add_node(T, "perc_render", "Page render / PDF raster", "ocr",
                      "PDF to image batch", 3, 1.2, 1.0);
    roe_tree_add_node(T, "perc_layout", "Layout analysis", "ocr",
                      "Reading order / blocks", 3, 1.5, 1.05);
    roe_tree_add_node(T, "perc_glyph", "Glyph / line detector", "ocr",
                      "Hermetic glyphs", 3, 1.0, 1.0);
    roe_tree_add_node(T, "perc_table", "Table structure", "ocr", "TEDS path", 3,
                      1.4, 1.05);
    roe_tree_add_node(T, "perc_formula", "Formula / code blocks", "ocr", "CDM",
                      3, 1.3, 1.0);
    roe_tree_add_node(T, "perc_multi", "Multi-page / long doc", "ocr",
                      "Stream pages", 3, 1.2, 1.0);

    roe_tree_add_node(T, "rec_hermetic", "Hermetic glyph OCR", "ocr",
                      "Offline free OCR floor", 3, 2.0, 1.1);
    roe_tree_add_node(T, "rec_classic", "Classic OCR engine", "ocr",
                      "tesseract-class", 3, 1.0, 1.0);
    roe_tree_add_node(T, "rec_vlm", "VLM OCR teacher", "ocr",
                      "Unlimited external teacher", 3, 1.5, 1.0);
    roe_tree_add_node(T, "rec_moe", "Long-context / R-SWA style", "ocr",
                      "Long decode", 3, 1.0, 1.0);
    roe_tree_add_node(T, "rec_lang", "Multi-language + scripts", "ocr",
                      "Scripts", 3, 1.0, 1.0);

    roe_tree_add_node(T, "ver_conf", "Conformal OCR abstain", "shell",
                      "Margin abstain", 3, 1.5, 1.1);
    roe_tree_add_node(T, "ver_gold", "Gold / consensus verify", "shell",
                      "Multi-vote promote", 3, 1.5, 1.1);
    roe_tree_add_node(T, "ver_capsule", "Vision capsule schema-2", "shell",
                      "CNU1/pack transport", 3, 1.4, 1.05);
    roe_tree_add_node(T, "ver_tidy", "Tidy vision/ocr taxonomy", "shell",
                      "cat/sub tidy", 3, 1.2, 1.0);
    roe_tree_add_node(T, "ver_proj", "Project doc memory L3", "shell",
                      "Doc L3 warm", 3, 1.6, 1.1);

    roe_tree_add_node(T, "bench_omni", "OmniDocBench eval harness", "meta",
                      "Official Overall", 3, 1.5, 1.0);
    roe_tree_add_node(T, "beat_quality", "Beat quality SOTA", "meta",
                      "WITHHELD unless Overall", 3, 2.0, 1.0);
    roe_tree_add_node(T, "beat_long", "Beat long-doc speed/mem", "meta",
                      "Stream + L3", 3, 1.2, 1.0);
    roe_tree_add_node(T, "beat_cost", "Beat $ / token / offline", "meta",
                      "teacher_rate KPI", 3, 1.8, 1.15);
    roe_tree_add_node(T, "beat_audit", "Beat audit / revoke / law", "meta",
                      "CERT + revoke", 3, 1.8, 1.15);
    roe_tree_add_node(T, "beat_system", "Beat as product system", "meta",
                      "Packs + hybrid", 3, 2.0, 1.2);

    roe_tree_add_prereq(T, "perc_render", "root_doc", 1);
    roe_tree_add_prereq(T, "perc_layout", "perc_render", 1);
    roe_tree_add_prereq(T, "perc_glyph", "perc_layout", 1);
    roe_tree_add_prereq(T, "perc_table", "perc_layout", 2);
    roe_tree_add_prereq(T, "perc_formula", "perc_glyph", 1);
    roe_tree_add_prereq(T, "perc_multi", "perc_layout", 2);
    roe_tree_add_prereq(T, "perc_multi", "rec_moe", 1);
    roe_tree_add_prereq(T, "rec_hermetic", "root_doc", 1);
    roe_tree_add_prereq(T, "rec_classic", "perc_glyph", 1);
    roe_tree_add_prereq(T, "rec_vlm", "perc_render", 2);
    roe_tree_add_prereq(T, "rec_moe", "rec_vlm", 1);
    roe_tree_add_prereq(T, "rec_lang", "rec_vlm", 1);
    roe_tree_add_prereq(T, "ver_conf", "root_shell", 1);
    roe_tree_add_prereq(T, "ver_gold", "ver_conf", 1);
    roe_tree_add_prereq(T, "ver_capsule", "root_shell", 1);
    roe_tree_add_prereq(T, "ver_tidy", "ver_capsule", 1);
    roe_tree_add_prereq(T, "ver_proj", "ver_tidy", 1);
    roe_tree_add_prereq(T, "ver_proj", "ver_gold", 1);
    roe_tree_add_prereq(T, "bench_omni", "root_sys", 1);
    roe_tree_add_prereq(T, "bench_omni", "perc_table", 1);
    roe_tree_add_prereq(T, "beat_quality", "bench_omni", 2);
    roe_tree_add_prereq(T, "beat_quality", "rec_moe", 2);
    roe_tree_add_prereq(T, "beat_quality", "perc_table", 2);
    roe_tree_add_prereq(T, "beat_long", "perc_multi", 2);
    roe_tree_add_prereq(T, "beat_long", "rec_moe", 2);
    roe_tree_add_prereq(T, "beat_cost", "rec_hermetic", 2);
    roe_tree_add_prereq(T, "beat_cost", "ver_proj", 1);
    roe_tree_add_prereq(T, "beat_cost", "rec_vlm", 1);
    roe_tree_add_prereq(T, "beat_audit", "ver_gold", 2);
    roe_tree_add_prereq(T, "beat_audit", "ver_capsule", 2);
    roe_tree_add_prereq(T, "beat_system", "beat_cost", 1);
    roe_tree_add_prereq(T, "beat_system", "beat_audit", 1);
    roe_tree_add_prereq(T, "beat_system", "ver_proj", 2);
    return 0;
}

void roe_self_init(RoeSelfModel *M) {
    if (!M) return;
    memset(M, 0, sizeof *M);
    snprintf(M->out_dir, sizeof M->out_dir, "%s", "artifacts/roe_self_model");
}

void roe_self_bind_roe(RoeSelfModel *M, RoeAsi *R) {
    if (M) M->roe = R;
}
void roe_self_bind_doc(RoeSelfModel *M, RoeDocAsset *D) {
    if (M) M->doc = D;
}
void roe_self_bind_goal(RoeSelfModel *M, RoeGoalEngine *G) {
    if (M) M->goal = G;
}
void roe_self_bind_tree(RoeSelfModel *M, RoeTree *T) {
    if (!M) return;
    M->tree = T;
    M->own_tree = 0;
}
void roe_self_set_out_dir(RoeSelfModel *M, const char *dir) {
    if (M && dir) snprintf(M->out_dir, sizeof M->out_dir, "%s", dir);
}
void roe_self_set_repo_root(RoeSelfModel *M, const char *root) {
    if (M && root) snprintf(M->repo_root, sizeof M->repo_root, "%s", root);
}
void roe_self_set_record_thoughts(RoeSelfModel *M, int on) {
    if (M) M->record_thoughts = on ? 1 : 0;
}

int roe_self_ensure_ocr_tree(RoeSelfModel *M) {
    if (!M) return -1;
    if (M->tree) return 0;
    if (seed_ocr_tree(&M->owned_tree) != 0) return -1;
    M->tree = &M->owned_tree;
    M->own_tree = 1;
    return 0;
}

int roe_self_apply_gate_evidence(RoeSelfModel *M) {
    char p[ROE_SELF_PATH];
    RoeTree *T;
    RoeTreeEvidenceRow tmp_rows[ROE_SELF_MAX_NODES];
    size_t ntmp = 0;
    if (!M) return -1;
    if (roe_self_ensure_ocr_tree(M) != 0) return -1;
    T = M->tree;

    /* Floor roots always known as product intent (not CERT of quality). */
    set_rank_ev(T, "root_doc", 1, "doctrine:doc_domain", tmp_rows, &ntmp,
                ROE_SELF_MAX_NODES);
    set_rank_ev(T, "root_shell", 2, "doctrine:never_self_cert", tmp_rows, &ntmp,
                ROE_SELF_MAX_NODES);
    set_rank_ev(T, "root_sys", 1, "doctrine:product_system", tmp_rows, &ntmp,
                ROE_SELF_MAX_NODES);

    join_root(M, p, sizeof p, "logs/roe_asi.log");
    if (file_contains(p, "ROE_ASI_PASS")) {
        set_rank_ev(T, "ver_conf", 1, "ROE_ASI_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "ver_gold", 1, "ROE_ASI_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "beat_audit", 1, "ROE_ASI_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }
    join_root(M, p, sizeof p, "logs/roe_asi_ocr.log");
    if (file_contains(p, "ROE_ASI_OCR_PASS")) {
        set_rank_ev(T, "perc_glyph", 1, "ROE_ASI_OCR_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "rec_hermetic", 3, "ROE_ASI_OCR_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "ver_conf", 1, "ROE_ASI_OCR_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }
    join_root(M, p, sizeof p, "logs/roe_asi_goal.log");
    if (file_contains(p, "ROE_ASI_GOAL_PASS")) {
        set_rank_ev(T, "ver_tidy", 2, "ROE_ASI_GOAL_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "ver_capsule", 1, "ROE_ASI_GOAL_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }
    join_root(M, p, sizeof p, "logs/roe_asi_ocr_asset.log");
    if (file_contains(p, "ROE_ASI_OCR_ASSET_PASS")) {
        set_rank_ev(T, "ver_proj", 2, "ROE_ASI_OCR_ASSET_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "ver_capsule", 2, "ROE_ASI_OCR_ASSET_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "beat_cost", 1, "ROE_ASI_OCR_ASSET_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "beat_system", 1, "ROE_ASI_OCR_ASSET_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }
    join_root(M, p, sizeof p, "logs/roe_asi_ocr_local.log");
    if (file_contains(p, "ROE_ASI_OCR_LOCAL_PASS")) {
        set_rank_ev(T, "ver_proj", 2, "ROE_ASI_OCR_LOCAL_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "beat_cost", 2, "ROE_ASI_OCR_LOCAL_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "perc_multi", 1, "ROE_ASI_OCR_LOCAL_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }
    join_root(M, p, sizeof p, "logs/roe_asi_ocr_tables.log");
    if (file_contains(p, "ROE_ASI_OCR_TABLES_PASS")) {
        set_rank_ev(T, "perc_table", 2, "ROE_ASI_OCR_TABLES_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "perc_layout", 1, "ROE_ASI_OCR_TABLES_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }
    join_root(M, p, sizeof p, "logs/roe_asi_ocr_bigfile.log");
    if (file_contains(p, "ROE_ASI_OCR_BIGFILE_PASS")) {
        set_rank_ev(T, "perc_multi", 2, "ROE_ASI_OCR_BIGFILE_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "beat_long", 1, "ROE_ASI_OCR_BIGFILE_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }
    join_root(M, p, sizeof p, "logs/roe_asi_ocr_surpass.log");
    if (file_contains(p, "ROE_ASI_OCR_SURPASS_PASS")) {
        set_rank_ev(T, "rec_vlm", 2, "ROE_ASI_OCR_SURPASS_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "ver_gold", 2, "ROE_ASI_OCR_SURPASS_PASS", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }
    join_root(M, p, sizeof p, "artifacts/omnidoc_full/OFFICIAL_FULL_CDM_REPORT.json");
    if (file_exists(p)) {
        set_rank_ev(T, "bench_omni", 2, "OFFICIAL_FULL_CDM_REPORT", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        /* Quality beat stays 0 unless Overall claim file asserts beat — never auto. */
        set_rank_ev(T, "rec_vlm", 2, "OFFICIAL_FULL_CDM_REPORT", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "perc_render", 1, "OFFICIAL_FULL_CDM_REPORT", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }
    join_root(M, p, sizeof p, "artifacts/omnidoc_full/beat_unlimited_report.json");
    if (file_contains(p, "beat_unlimited") && file_contains(p, "true")) {
        /* beat Unlimited overall — still not board SOTA */
        set_rank_ev(T, "beat_system", 2, "beat_unlimited_report", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
        set_rank_ev(T, "beat_cost", 2, "beat_unlimited_report", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }
    join_root(M, p, sizeof p, "artifacts/roe_ocr_local/teacher_rate_kpi.json");
    if (file_exists(p)) {
        set_rank_ev(T, "beat_cost", 2, "teacher_rate_kpi", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }
    /* beat_quality deliberately stays 0 without explicit SOTA claim file */
    join_root(M, p, sizeof p, "artifacts/omnidoc_full/SOTA_CLAIM.txt");
    if (file_contains(p, "OVERALL_GE_LEADERBOARD=1")) {
        set_rank_ev(T, "beat_quality", 2, "SOTA_CLAIM", tmp_rows, &ntmp,
                    ROE_SELF_MAX_NODES);
    }

    /* Audit floor from shell law always partial */
    set_rank_ev(T, "beat_audit", 2, "shell_law_encoded", tmp_rows, &ntmp,
                ROE_SELF_MAX_NODES);

    (void)tmp_rows;
    (void)ntmp;
    if (M->out_dir[0]) {
        roe_tree_set_catalog(T, M->out_dir);
        (void)roe_tree_save(T);
    }
    return 0;
}

int roe_self_health_scan(const RoeAsi *R, RoeSkillHealth *out, size_t cap,
                         size_t *n_out, size_t *n_ready) {
    size_t i, n = 0, ready = 0;
    if (!R || !out || !cap) return -1;
    for (i = 0; i < R->n_skills && n < cap; i++) {
        const RoeSkill *s = &R->skills[i];
        RoeSkillHealth *h = &out[n];
        int k;
        if (!s->active) continue;
        memset(h, 0, sizeof *h);
        snprintf(h->skill_id, sizeof h->skill_id, "%s", s->id);
        h->certified = s->certified;
        h->active = s->active;
        h->hits = s->hits;
        for (k = 0; k < 5; k++) {
            h->layer[k] = ROE_HL_SKIP;
            snprintf(h->reason[k], sizeof h->reason[k], "%s", "skip");
        }
        /* 0 REGISTRY */
        if (s->id[0]) {
            h->layer[0] = ROE_HL_PASS;
            snprintf(h->reason[0], sizeof h->reason[0], "%s", "named");
        } else {
            h->layer[0] = ROE_HL_FAIL;
            snprintf(h->reason[0], sizeof h->reason[0], "%s", "no_id");
        }
        /* 1 LOADABLE */
        if (h->layer[0] == ROE_HL_PASS) {
            if (s->pattern[0] && s->answer[0]) {
                h->layer[1] = ROE_HL_PASS;
                snprintf(h->reason[1], sizeof h->reason[1], "%s", "shape_ok");
            } else {
                h->layer[1] = ROE_HL_FAIL;
                snprintf(h->reason[1], sizeof h->reason[1], "%s", "empty");
            }
        }
        /* 2 EXECUTION — invocable shape (certified or has pattern) */
        if (h->layer[1] == ROE_HL_PASS) {
            h->layer[2] = ROE_HL_PASS;
            snprintf(h->reason[2], sizeof h->reason[2], "%s",
                     s->hits > 0 ? "hit" : "ready");
        }
        /* 3 SEMANTIC — certified only */
        if (h->layer[2] == ROE_HL_PASS) {
            if (s->certified) {
                h->layer[3] = ROE_HL_PASS;
                snprintf(h->reason[3], sizeof h->reason[3], "%s", "certified");
            } else {
                h->layer[3] = ROE_HL_FAIL;
                snprintf(h->reason[3], sizeof h->reason[3], "%s", "uncert");
            }
        }
        /* 4 UTILITY — evidence of use or fresh cert with privilege path */
        if (h->layer[3] == ROE_HL_PASS) {
            if (s->hits >= 1 || s->certified) {
                h->layer[4] = ROE_HL_PASS;
                snprintf(h->reason[4], sizeof h->reason[4], "%s",
                         s->hits >= 1 ? "used" : "fresh_cert");
            } else {
                h->layer[4] = ROE_HL_FAIL;
                snprintf(h->reason[4], sizeof h->reason[4], "%s", "no_util");
            }
        }
        h->deepest_pass = -1;
        for (k = 0; k < 5; k++) {
            if (h->layer[k] == ROE_HL_PASS)
                h->deepest_pass = k;
            else
                break;
        }
        h->production_ready = (h->deepest_pass == 4) ? 1 : 0;
        if (h->production_ready) ready++;
        n++;
    }
    if (n_out) *n_out = n;
    if (n_ready) *n_ready = ready;
    return 0;
}

static void fill_econ(const RoeAsi *R, RoeSelfReport *rep) {
    size_t i;
    uint64_t used, base;
    if (!R || !rep) return;
    rep->n_turns = R->n_turns;
    rep->n_local_hit = R->n_local_hit;
    rep->n_lookup_hit = R->n_lookup_hit;
    rep->n_llm_call = R->n_llm_call;
    rep->n_abstain = R->n_abstain;
    rep->n_ask_user = R->n_ask_user;
    rep->n_promote = R->n_promote;
    rep->n_verify_fail = R->n_verify_fail;
    used = R->tokens_local + R->tokens_lookup + R->tokens_llm;
    base = R->tokens_baseline_llm;
    rep->tokens_used = used;
    rep->tokens_baseline = base;
    rep->local_hit_rate =
        R->n_turns ? (double)R->n_local_hit / (double)R->n_turns : 0.0;
    rep->token_save_ratio =
        base ? 1.0 - (double)used / (double)base : 0.0;
    rep->n_skills_active = 0;
    rep->n_skills_cert = 0;
    for (i = 0; i < R->n_skills; i++) {
        if (!R->skills[i].active) continue;
        rep->n_skills_active++;
        if (R->skills[i].certified) rep->n_skills_cert++;
    }
    rep->n_pending = R->n_pending;
}

int roe_self_snapshot(RoeSelfModel *M, RoeSelfReport *rep) {
    size_t i;
    if (!M || !rep || !M->roe) return -1;
    memset(rep, 0, sizeof *rep);
    rep->never_self_cert = 1;
    rep->second_brain = 0;
    snprintf(rep->law_line, sizeof rep->law_line, "%s",
             "LOCAL CERT first; teacher untrusted until verify; never self-CERT");

    fill_econ(M->roe, rep);

    if (M->doc) {
        rep->has_doc = 1;
        roe_doc_coverage(M->doc, &rep->coverage);
    }

    (void)roe_self_ensure_ocr_tree(M);
    if (M->tree) {
        rep->has_tree = 1;
        (void)roe_self_apply_gate_evidence(M);
        roe_tree_snapshot(M->tree, &rep->tree_snap);
        rep->n_tree_rows = 0;
        for (i = 0; i < M->tree->n_nodes && rep->n_tree_rows < ROE_SELF_MAX_NODES;
             i++) {
            const RoeTreeNode *n = &M->tree->nodes[i];
            RoeTreeEvidenceRow *r;
            if (!n->active) continue;
            r = &rep->tree_rows[rep->n_tree_rows++];
            memset(r, 0, sizeof *r);
            snprintf(r->node_id, sizeof r->node_id, "%s", n->id);
            r->rank = n->rank;
            r->max_rank = n->max_rank;
            snprintf(r->evidence, sizeof r->evidence, "%s",
                     n->rank > 0 ? "gate_or_doctrine" : "none");
        }
    }

    (void)roe_self_health_scan(M->roe, rep->health, ROE_SELF_MAX_SKILLS,
                               &rep->n_health, &rep->n_production_ready);

    snprintf(rep->inventory_line, sizeof rep->inventory_line,
             "hit=%.2f save=%.2f cert=%zu/%zu ready=%zu abstain=%llu "
             "doc=%d tree_unlock=%d never_self_cert=1 second_brain=0",
             rep->local_hit_rate, rep->token_save_ratio, rep->n_skills_cert,
             rep->n_skills_active, rep->n_production_ready,
             (unsigned long long)rep->n_abstain, rep->has_doc,
             rep->has_tree ? rep->tree_snap.nodes_unlocked : 0);

    snprintf(rep->summary, sizeof rep->summary,
             "ROE self: hit=%.2f cert=%zu ready=%zu tree=%d goal=%s law=ok",
             rep->local_hit_rate, rep->n_skills_cert, rep->n_production_ready,
             rep->has_tree ? rep->tree_snap.nodes_unlocked : 0,
             rep->has_goal ? (rep->goal_miss == 0 ? "HAVE" : "MISS")
                           : "none");

    if (M->record_thoughts) (void)roe_self_record_thought(M, rep);
    return 0;
}

int roe_self_goal_probe(RoeSelfModel *M, const char *goal, RoeSelfReport *rep) {
    RoeGoalPlan plan;
    int i, nsplit;
    if (!M || !rep) return -1;
    if (!M->goal) return -2;
    if (!goal || !goal[0]) goal = "ocr document with local packs and abstain";
    memset(&plan, 0, sizeof plan);
    nsplit = roe_goal_microsplit(M->goal, goal, &plan);
    if (nsplit < 0) return -3;
    /* Execute without auto-learn (HAVE/MISS only). */
    (void)roe_goal_run(M->goal, goal, 0, &plan);
    rep->has_goal = 1;
    snprintf(rep->goal_text, sizeof rep->goal_text, "%s", goal);
    rep->goal_have = rep->goal_miss = rep->goal_learned = rep->goal_blocked = 0;
    for (i = 0; i < plan.n_splits; i++) {
        switch (plan.splits[i].status) {
        case ROE_GOAL_HAVE:
            rep->goal_have++;
            break;
        case ROE_GOAL_LEARNED:
            rep->goal_learned++;
            break;
        case ROE_GOAL_MISS:
            rep->goal_miss++;
            break;
        case ROE_GOAL_BLOCKED:
            rep->goal_blocked++;
            break;
        default:
            break;
        }
    }
    /* refresh summary bit */
    snprintf(rep->summary, sizeof rep->summary,
             "ROE self: H=%d M=%d L=%d B=%d hit=%.2f cert=%zu",
             rep->goal_have, rep->goal_miss, rep->goal_learned, rep->goal_blocked,
             rep->local_hit_rate, rep->n_skills_cert);
    return 0;
}

int roe_self_export_pack(const RoeSelfModel *M, const RoeSelfReport *rep,
                         const char *out_dir) {
    char path[ROE_SELF_PATH];
    FILE *f;
    size_t i;
    const char *dir = out_dir;
    if (!rep) return -1;
    if (!dir && M) dir = M->out_dir;
    if (!dir || !dir[0]) return -1;
    mkdir_p(dir);

    if (path_join2(path, sizeof path, dir, "SELF.abi") != 0) return -5;
    f = fopen(path, "w");
    if (!f) return -2;
    fprintf(f, "%s\n", ROE_SELF_ABI_MAGIC);
    fprintf(f, "abi_version %d\n", ROE_SELF_ABI_VER);
    fprintf(f, "kind roe_self_model\n");
    fprintf(f, "second_brain 0\n");
    fprintf(f, "never_self_cert 1\n");
    fprintf(f, "skills_cert %zu\n", rep->n_skills_cert);
    fprintf(f, "production_ready %zu\n", rep->n_production_ready);
    fprintf(f, "tree_unlocked %d\n",
            rep->has_tree ? rep->tree_snap.nodes_unlocked : 0);
    fclose(f);

    if (path_join2(path, sizeof path, dir, "self_report.json") != 0) return -5;
    f = fopen(path, "w");
    if (!f) return -3;
    fprintf(f, "{\n");
    fprintf(f, "  \"never_self_cert\": true,\n");
    fprintf(f, "  \"second_brain\": false,\n");
    fprintf(f, "  \"local_hit_rate\": %.6f,\n", rep->local_hit_rate);
    fprintf(f, "  \"token_save_ratio\": %.6f,\n", rep->token_save_ratio);
    fprintf(f, "  \"n_turns\": %llu,\n", (unsigned long long)rep->n_turns);
    fprintf(f, "  \"n_local_hit\": %llu,\n", (unsigned long long)rep->n_local_hit);
    fprintf(f, "  \"n_llm_call\": %llu,\n", (unsigned long long)rep->n_llm_call);
    fprintf(f, "  \"n_abstain\": %llu,\n", (unsigned long long)rep->n_abstain);
    fprintf(f, "  \"n_skills_cert\": %zu,\n", rep->n_skills_cert);
    fprintf(f, "  \"n_skills_active\": %zu,\n", rep->n_skills_active);
    fprintf(f, "  \"n_production_ready\": %zu,\n", rep->n_production_ready);
    fprintf(f, "  \"has_doc\": %s,\n", rep->has_doc ? "true" : "false");
    if (rep->has_doc) {
        fprintf(f, "  \"coverage\": {\n");
        fprintf(f, "    \"n_pages\": %d,\n", rep->coverage.n_pages);
        fprintf(f, "    \"n_l3\": %d,\n", rep->coverage.n_l3);
        fprintf(f, "    \"n_local_pdf\": %d,\n", rep->coverage.n_local_pdf);
        fprintf(f, "    \"n_teacher\": %d,\n", rep->coverage.n_teacher);
        fprintf(f, "    \"n_abstain\": %d,\n", rep->coverage.n_abstain);
        fprintf(f, "    \"teacher_rate\": %.6f,\n", rep->coverage.teacher_rate);
        fprintf(f, "    \"local_rate\": %.6f,\n", rep->coverage.local_rate);
        fprintf(f, "    \"kpi_teacher_under_10pct\": %d\n",
                rep->coverage.kpi_teacher_under_10pct);
        fprintf(f, "  },\n");
    }
    fprintf(f, "  \"has_goal\": %s,\n", rep->has_goal ? "true" : "false");
    if (rep->has_goal) {
        fprintf(f, "  \"goal\": {\n");
        fprintf(f, "    \"text\": \"%s\",\n", rep->goal_text);
        fprintf(f, "    \"have\": %d,\n", rep->goal_have);
        fprintf(f, "    \"miss\": %d,\n", rep->goal_miss);
        fprintf(f, "    \"learned\": %d,\n", rep->goal_learned);
        fprintf(f, "    \"blocked\": %d\n", rep->goal_blocked);
        fprintf(f, "  },\n");
    }
    fprintf(f, "  \"has_tree\": %s,\n", rep->has_tree ? "true" : "false");
    if (rep->has_tree) {
        fprintf(f, "  \"tree\": {\n");
        fprintf(f, "    \"nodes_unlocked\": %d,\n", rep->tree_snap.nodes_unlocked);
        fprintf(f, "    \"nodes_total\": %d,\n", rep->tree_snap.nodes_total);
        fprintf(f, "    \"total_power\": %.4f,\n", rep->tree_snap.total_power);
        fprintf(f, "    \"summary\": \"%s\",\n", rep->tree_snap.summary);
        fprintf(f, "    \"nodes\": [\n");
        for (i = 0; i < rep->n_tree_rows; i++) {
            const RoeTreeEvidenceRow *r = &rep->tree_rows[i];
            fprintf(f,
                    "      {\"id\":\"%s\",\"rank\":%d,\"max\":%d,\"evidence\":\"%s\"}%s\n",
                    r->node_id, r->rank, r->max_rank, r->evidence,
                    (i + 1 < rep->n_tree_rows) ? "," : "");
        }
        fprintf(f, "    ]\n");
        fprintf(f, "  },\n");
    }
    fprintf(f, "  \"health\": [\n");
    for (i = 0; i < rep->n_health; i++) {
        const RoeSkillHealth *h = &rep->health[i];
        fprintf(f,
                "    {\"id\":\"%s\",\"certified\":%s,\"hits\":%llu,\"deepest\":%d,"
                "\"production_ready\":%s}%s\n",
                h->skill_id, h->certified ? "true" : "false",
                (unsigned long long)h->hits, h->deepest_pass,
                h->production_ready ? "true" : "false",
                (i + 1 < rep->n_health) ? "," : "");
    }
    fprintf(f, "  ],\n");
    fprintf(f, "  \"inventory_line\": \"%s\",\n", rep->inventory_line);
    fprintf(f, "  \"law\": \"%s\",\n", rep->law_line);
    fprintf(f, "  \"summary\": \"%s\"\n", rep->summary);
    fprintf(f, "}\n");
    fclose(f);

    if (path_join2(path, sizeof path, dir, "MANIFEST.txt") != 0) return -5;
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "ROE self-model pack\n");
        fprintf(f, "not_a_second_brain 1\n");
        fprintf(f, "never_self_cert 1\n");
        fprintf(f, "instrumented_inventory 1\n");
        fprintf(f, "%s\n", rep->summary);
        fclose(f);
    }

    /* Also export skill catalog if roe bound with catalog dir */
    if (M && M->roe && M->roe->catalog_dir[0]) {
        (void)roe_save_catalog(M->roe);
    }
    if (M && M->tree) {
        roe_tree_set_catalog(M->tree, dir);
        (void)roe_tree_save(M->tree);
    }
    if (M && M->doc) {
        char dpack[ROE_SELF_PATH];
        if (path_join2(dpack, sizeof dpack, dir, "doc_pack") == 0)
            (void)roe_doc_pack_export(M->doc, dpack);
    }
    return 0;
}

int roe_self_pack_validate(const char *pack_dir) {
    char path[ROE_SELF_PATH], line[256];
    FILE *f;
    int ver = -1;
    int magic_ok = 0;
    if (!pack_dir) return 0;
    if (path_join2(path, sizeof path, pack_dir, "SELF.abi") != 0) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, ROE_SELF_ABI_MAGIC, strlen(ROE_SELF_ABI_MAGIC)) == 0)
            magic_ok = 1;
        sscanf(line, "abi_version %d", &ver);
    }
    fclose(f);
    if (!magic_ok || ver != ROE_SELF_ABI_VER) return 0;
    if (path_join2(path, sizeof path, pack_dir, "self_report.json") != 0) return 0;
    return file_exists(path) ? 1 : 0;
}

void roe_self_dump(const RoeSelfReport *rep, char *buf, size_t cap) {
    if (!rep || !buf || !cap) return;
    snprintf(buf, cap,
             "self: hit=%.1f%% save=%.1f%% cert=%zu ready=%zu abstain=%llu "
             "tree=%d/%d doc=%d goal_miss=%d law=never_self_cert",
             100.0 * rep->local_hit_rate, 100.0 * rep->token_save_ratio,
             rep->n_skills_cert, rep->n_production_ready,
             (unsigned long long)rep->n_abstain,
             rep->has_tree ? rep->tree_snap.nodes_unlocked : 0,
             rep->has_tree ? rep->tree_snap.nodes_total : 0, rep->has_doc,
             rep->goal_miss);
}

int roe_self_record_thought(const RoeSelfModel *M, const RoeSelfReport *rep) {
    char line[ROE_SELF_SUMMARY + 64];
    if (!M || !rep || !M->record_thoughts) return -1;
    (void)agent_memory_init();
    snprintf(line, sizeof line, "[ROE-self] %s", rep->summary);
    return agent_record_thought(line);
}
