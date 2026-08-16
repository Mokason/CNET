#ifndef CNET_OBSIDIAN_LEARN_H
#define CNET_OBSIDIAN_LEARN_H

/* Harvest structured learnables from an Obsidian vault into CORE evolve inputs.
 *
 * Only structured content becomes CERT fuel (never free prose auto-CERT):
 *   - teach TAG n m
 *   - TAG n = m
 *   - pairs=0:1,1:2,... (16 complete → domain)
 *   - given domain TAG pairs=...
 *   - fenced ```cnet-domain blocks
 *   - YAML: cnet_domain: TAG
 *
 * Writes:
 *   - typed rows into miss_log (for live 16/16 admit)
 *   - optional pending_goals.txt lines
 *
 * Gate: make cnet_obsidian_learn
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int files_scanned;
    int files_hit;
    int teaches;
    int pair_lines;
    int domains_complete_emitted;
    int goals_queued;
    int skipped;
    char vault[512];
    char miss_path[512];
} CnetObsidianLearnReport;

/* Resolve vault: CNET_OBSIDIAN_VAULT, OBSIDIAN_VAULT_PATH, ~/Obsidian, ~/obsidian-vault */
int cnet_obsidian_resolve_vault(char *out, size_t cap);

/* Scan vault markdown → append teach/pairs into miss_log; queue goals if any.
 * max_files caps walk. Returns 0 ok. */
int cnet_obsidian_learn(const char *vault, const char *miss_path,
                        const char *bricks_dir, int max_files,
                        CnetObsidianLearnReport *rep);

#ifdef __cplusplus
}
#endif

#endif
