#include "../include/cnet_roe_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static RoeTreeNode *find_mut(RoeTree *T, const char *id) {
    size_t i;
    if (!T || !id) return NULL;
    for (i = 0; i < T->n_nodes; i++)
        if (T->nodes[i].active && strcmp(T->nodes[i].id, id) == 0)
            return &T->nodes[i];
    return NULL;
}

static const RoeTreeNode *find_c(const RoeTree *T, const char *id) {
    size_t i;
    if (!T || !id) return NULL;
    for (i = 0; i < T->n_nodes; i++)
        if (T->nodes[i].active && strcmp(T->nodes[i].id, id) == 0)
            return &T->nodes[i];
    return NULL;
}

static int mkdir_p(const char *path) {
    char tmp[ROE_TREE_PATH];
    size_t i, len;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return mkdir(tmp, 0755);
}

void roe_tree_init(RoeTree *T) {
    if (!T) return;
    memset(T, 0, sizeof *T);
}

void roe_tree_set_catalog(RoeTree *T, const char *dir) {
    if (!T || !dir) return;
    snprintf(T->catalog_dir, sizeof T->catalog_dir, "%s", dir);
}

int roe_tree_add_node(RoeTree *T, const char *id, const char *title,
                      const char *branch, const char *desc, int max_rank,
                      double power, double synergy) {
    RoeTreeNode *n;
    if (!T || !id || !title || !branch) return -1;
    if (find_mut(T, id)) return -2;
    if (T->n_nodes >= ROE_TREE_MAX_NODES) return -1;
    n = &T->nodes[T->n_nodes++];
    memset(n, 0, sizeof *n);
    snprintf(n->id, sizeof n->id, "%s", id);
    snprintf(n->title, sizeof n->title, "%s", title);
    snprintf(n->branch, sizeof n->branch, "%s", branch);
    if (desc) snprintf(n->desc, sizeof n->desc, "%s", desc);
    n->max_rank = max_rank > 0 ? max_rank : 1;
    n->rank = 0;
    n->power = power;
    n->synergy = synergy > 0.0 ? synergy : 1.0;
    n->active = 1;
    return 0;
}

int roe_tree_add_prereq(RoeTree *T, const char *id, const char *pre_id,
                        int pre_rank_need) {
    RoeTreeNode *n = find_mut(T, id);
    if (!n || !pre_id) return -1;
    if (n->n_pre >= ROE_TREE_MAX_PREREQ) return -1;
    snprintf(n->pre_id[n->n_pre], sizeof n->pre_id[0], "%s", pre_id);
    n->pre_rank[n->n_pre] = pre_rank_need > 0 ? pre_rank_need : 1;
    n->n_pre++;
    return 0;
}

int roe_tree_add_tag(RoeTree *T, const char *id, const char *tag) {
    RoeTreeNode *n = find_mut(T, id);
    if (!n || !tag) return -1;
    if (n->n_tags >= ROE_TREE_MAX_TAGS) return -1;
    snprintf(n->tags[n->n_tags], sizeof n->tags[0], "%s", tag);
    n->n_tags++;
    return 0;
}

int roe_tree_seed_default(RoeTree *T) {
    if (!T) return -1;
    /* roots */
    roe_tree_add_node(T, "root_shell", "Shell Discipline", "shell",
                      "Fail-closed, verify before admit", 3, 2.0, 1.05);
    roe_tree_add_node(T, "root_coding", "Coding Fundamentals", "coding",
                      "Basic language patterns", 5, 3.0, 1.08);
    roe_tree_add_node(T, "root_debug", "Debug Fundamentals", "debug",
                      "Read errors and checklists", 5, 3.0, 1.08);
    roe_tree_add_node(T, "root_meta", "Meta Learning", "meta",
                      "Turn misses into lasting skills", 5, 4.0, 1.12);

    /* coding branch */
    roe_tree_add_node(T, "code_syntax", "Syntax Basics", "coding",
                      "hello/assign/if", 3, 2.0, 1.02);
    roe_tree_add_node(T, "code_control", "Control Flow", "coding",
                      "for/while/bool", 3, 2.5, 1.04);
    roe_tree_add_node(T, "code_data", "Data Structures", "coding",
                      "list/dict/str", 3, 2.5, 1.04);
    roe_tree_add_node(T, "code_functions", "Functions & Modules", "coding",
                      "def/import/files", 3, 3.0, 1.06);
    roe_tree_add_node(T, "code_errors", "Error Handling", "coding",
                      "try/except", 2, 2.0, 1.03);
    roe_tree_add_node(T, "code_oop", "Objects", "coding", "class/init", 2, 2.5,
                      1.05);
    roe_tree_add_node(T, "code_advanced", "Advanced Patterns", "coding",
                      "recursion and composition", 3, 3.5, 1.08);

    /* debug branch */
    roe_tree_add_node(T, "dbg_read_tb", "Read Tracebacks", "debug",
                      "L1 error recipes", 3, 2.5, 1.04);
    roe_tree_add_node(T, "dbg_checklist", "Debug Checklist", "debug",
                      "L2 procedures", 2, 2.0, 1.05);
    roe_tree_add_node(T, "dbg_pytest", "Test Loop", "debug", "red/green verify", 3,
                      3.0, 1.07);
    roe_tree_add_node(T, "dbg_project_mem", "Project Memory", "debug",
                      "L3 project-scoped fixes", 5, 5.0, 1.15);
    roe_tree_add_node(T, "dbg_isolation", "Project Isolation", "debug",
                      "No cross-project leak", 2, 2.0, 1.06);
    roe_tree_add_node(T, "dbg_mastery", "Debug Mastery", "debug",
                      "Compound multi-project competence", 3, 6.0, 1.2);

    /* meta / shell */
    roe_tree_add_node(T, "meta_promote", "Promote Discipline", "meta",
                      "Votes/verify before CERT", 3, 3.0, 1.1);
    roe_tree_add_node(T, "meta_catalog", "Catalog Persistence", "meta",
                      "Save/load packs", 2, 2.0, 1.05);
    roe_tree_add_node(T, "meta_token", "Token Economy", "meta",
                      "Local hits crush LLM cost", 3, 4.0, 1.15);
    roe_tree_add_node(T, "meta_compound", "Compound Power", "meta",
                      "Tree synergies fully online", 3, 8.0, 1.25);

    /* prereqs — skill tree edges */
    roe_tree_add_prereq(T, "code_syntax", "root_coding", 1);
    roe_tree_add_prereq(T, "code_control", "code_syntax", 1);
    roe_tree_add_prereq(T, "code_data", "code_syntax", 1);
    roe_tree_add_prereq(T, "code_functions", "code_control", 1);
    roe_tree_add_prereq(T, "code_functions", "code_data", 1);
    roe_tree_add_prereq(T, "code_errors", "code_functions", 1);
    roe_tree_add_prereq(T, "code_oop", "code_functions", 1);
    roe_tree_add_prereq(T, "code_advanced", "code_errors", 1);
    roe_tree_add_prereq(T, "code_advanced", "code_oop", 1);

    roe_tree_add_prereq(T, "dbg_read_tb", "root_debug", 1);
    roe_tree_add_prereq(T, "dbg_checklist", "dbg_read_tb", 1);
    roe_tree_add_prereq(T, "dbg_pytest", "dbg_checklist", 1);
    roe_tree_add_prereq(T, "dbg_project_mem", "dbg_pytest", 1);
    roe_tree_add_prereq(T, "dbg_project_mem", "root_shell", 1);
    roe_tree_add_prereq(T, "dbg_isolation", "dbg_project_mem", 1);
    roe_tree_add_prereq(T, "dbg_mastery", "dbg_isolation", 2);
    roe_tree_add_prereq(T, "dbg_mastery", "code_advanced", 1);

    roe_tree_add_prereq(T, "meta_promote", "root_meta", 1);
    roe_tree_add_prereq(T, "meta_promote", "root_shell", 1);
    roe_tree_add_prereq(T, "meta_catalog", "meta_promote", 1);
    roe_tree_add_prereq(T, "meta_token", "meta_catalog", 1);
    roe_tree_add_prereq(T, "meta_compound", "meta_token", 2);
    roe_tree_add_prereq(T, "meta_compound", "dbg_mastery", 1);

    /* tags for routing/synergy display */
    roe_tree_add_tag(T, "code_syntax", "python");
    roe_tree_add_tag(T, "dbg_project_mem", "l3");
    roe_tree_add_tag(T, "meta_token", "economy");

    /* roots start available — rank 0 until first event; unlock roots freely via on_* */
    return 0;
}

int roe_tree_can_unlock(const RoeTree *T, const char *id) {
    const RoeTreeNode *n = find_c(T, id);
    int i;
    if (!n) return 0;
    if (n->rank >= n->max_rank) return 0;
    for (i = 0; i < n->n_pre; i++) {
        const RoeTreeNode *p = find_c(T, n->pre_id[i]);
        if (!p || p->rank < n->pre_rank[i]) return 0;
    }
    return 1;
}

int roe_tree_unlock(RoeTree *T, const char *id, const char *evidence_note) {
    RoeTreeNode *n;
    (void)evidence_note;
    if (!T || !id) return -1;
    T->n_events++;
    n = find_mut(T, id);
    if (!n) {
        T->n_unlock_blocked++;
        return -1;
    }
    if (!roe_tree_can_unlock(T, id)) {
        T->n_unlock_blocked++;
        return -1;
    }
    n->rank++;
    n->unlocks++;
    n->tick = ++T->tick;
    T->n_unlock_ok++;
    {
        RoeTreeSnapshot S;
        roe_tree_snapshot(T, &S);
        if (S.total_power > T->peak_power) T->peak_power = S.total_power;
    }
    if (T->catalog_dir[0]) (void)roe_tree_save(T);
    return n->rank;
}

/* try unlock root then node */
static int bump(RoeTree *T, const char *id) {
    if (roe_tree_can_unlock(T, id)) return roe_tree_unlock(T, id, "event");
    return -1;
}

int roe_tree_on_coding_skill(RoeTree *T, const char *skill_pattern) {
    const char *p = skill_pattern ? skill_pattern : "";
    bump(T, "root_coding");
    bump(T, "root_shell");
    if (strstr(p, "hello") || strstr(p, "variable") || strstr(p, "if else") ||
        strstr(p, "assign"))
        return bump(T, "code_syntax");
    if (strstr(p, "for") || strstr(p, "while") || strstr(p, "boolean"))
        return bump(T, "code_control");
    if (strstr(p, "list") || strstr(p, "dict") || strstr(p, "split") ||
        strstr(p, "string"))
        return bump(T, "code_data");
    if (strstr(p, "function") || strstr(p, "read file") || strstr(p, "write file") ||
        strstr(p, "def"))
        return bump(T, "code_functions");
    if (strstr(p, "try") || strstr(p, "except")) return bump(T, "code_errors");
    if (strstr(p, "class")) return bump(T, "code_oop");
    if (strstr(p, "recursion")) return bump(T, "code_advanced");
    /* generic coding progress still ranks syntax */
    return bump(T, "code_syntax");
}

int roe_tree_on_debug_l1(RoeTree *T, const char *error_name) {
    (void)error_name;
    bump(T, "root_debug");
    bump(T, "root_shell");
    return bump(T, "dbg_read_tb");
}

int roe_tree_on_debug_l3(RoeTree *T, const char *project_id) {
    (void)project_id;
    bump(T, "root_debug");
    bump(T, "root_shell");
    bump(T, "dbg_read_tb");
    bump(T, "dbg_checklist");
    bump(T, "dbg_pytest");
    bump(T, "meta_promote");
    {
        int r = bump(T, "dbg_project_mem");
        bump(T, "dbg_isolation");
        bump(T, "dbg_mastery");
        bump(T, "meta_catalog");
        bump(T, "meta_token");
        bump(T, "meta_compound");
        return r;
    }
}

int roe_tree_on_verify(RoeTree *T) {
    bump(T, "root_shell");
    bump(T, "root_meta");
    bump(T, "meta_promote");
    return bump(T, "dbg_pytest");
}

void roe_tree_snapshot(const RoeTree *T, RoeTreeSnapshot *S) {
    size_t i;
    double branch_pow[4];
    double branch_syn[4];
    int bi;
    if (!S) return;
    memset(S, 0, sizeof *S);
    if (!T) return;
    branch_pow[0] = branch_pow[1] = branch_pow[2] = branch_pow[3] = 0.0;
    branch_syn[0] = branch_syn[1] = branch_syn[2] = branch_syn[3] = 1.0;
    S->nodes_total = (int)T->n_nodes;
    for (i = 0; i < T->n_nodes; i++) {
        const RoeTreeNode *n = &T->nodes[i];
        double contrib;
        if (!n->active || n->rank <= 0) continue;
        S->nodes_unlocked++;
        S->ranks_total += n->rank;
        contrib = n->power * (double)n->rank;
        if (!strcmp(n->branch, "coding")) {
            bi = 0;
            branch_pow[0] += contrib;
            if (n->synergy > branch_syn[0]) branch_syn[0] = n->synergy;
        } else if (!strcmp(n->branch, "debug")) {
            bi = 1;
            branch_pow[1] += contrib;
            if (n->synergy > branch_syn[1]) branch_syn[1] = n->synergy;
        } else if (!strcmp(n->branch, "shell")) {
            bi = 2;
            branch_pow[2] += contrib;
            if (n->synergy > branch_syn[2]) branch_syn[2] = n->synergy;
        } else {
            bi = 3;
            branch_pow[3] += contrib;
            if (n->synergy > branch_syn[3]) branch_syn[3] = n->synergy;
        }
        (void)bi;
    }
    /* compound: branch power * max synergy in branch; then cross-branch multiply lightly */
    S->coding_power = branch_pow[0] * branch_syn[0];
    S->debug_power = branch_pow[1] * branch_syn[1];
    S->shell_power = branch_pow[2] * branch_syn[2];
    S->meta_power = branch_pow[3] * branch_syn[3];
    S->total_power =
        (S->coding_power + S->debug_power + S->shell_power + S->meta_power);
    /* cross-branch compound: if both coding+debug unlocked deep, bonus */
    if (S->coding_power > 5.0 && S->debug_power > 5.0)
        S->total_power *= 1.10;
    if (S->meta_power > 5.0 && S->shell_power > 2.0) S->total_power *= 1.08;
    snprintf(S->summary, sizeof S->summary,
             "power=%.1f (code=%.1f dbg=%.1f shell=%.1f meta=%.1f) unlocked=%d/%d "
             "ranks=%d",
             S->total_power, S->coding_power, S->debug_power, S->shell_power,
             S->meta_power, S->nodes_unlocked, S->nodes_total, S->ranks_total);
}

const RoeTreeNode *roe_tree_get(const RoeTree *T, const char *id) {
    return find_c(T, id);
}

int roe_tree_save(const RoeTree *T) {
    char path[ROE_TREE_PATH];
    FILE *f;
    size_t i, cl;
    if (!T || !T->catalog_dir[0]) return -1;
    mkdir_p(T->catalog_dir);
    cl = strlen(T->catalog_dir);
    if (cl + 15 >= sizeof path) return -5;
    memcpy(path, T->catalog_dir, cl);
    memcpy(path + cl, "/skill_tree.jsonl", 18);
    f = fopen(path, "w");
    if (!f) return -2;
    for (i = 0; i < T->n_nodes; i++) {
        const RoeTreeNode *n = &T->nodes[i];
        if (!n->active) continue;
        fprintf(f,
                "{\"id\":\"%s\",\"branch\":\"%s\",\"rank\":%d,\"max\":%d,\"power\":%.3f,"
                "\"syn\":%.3f,\"unlocks\":%llu}\n",
                n->id, n->branch, n->rank, n->max_rank, n->power, n->synergy,
                (unsigned long long)n->unlocks);
    }
    fclose(f);
    /* snapshot */
    {
        RoeTreeSnapshot S;
        roe_tree_snapshot(T, &S);
        memcpy(path + cl, "/power.txt", 11);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "%s\npeak=%.3f\n", S.summary, T->peak_power);
            fclose(f);
        }
    }
    return (int)T->n_nodes;
}

int roe_tree_load(RoeTree *T) {
    char path[ROE_TREE_PATH], line[512];
    FILE *f;
    size_t cl;
    int n = 0;
    if (!T || !T->catalog_dir[0]) return -1;
    if (T->n_nodes == 0) roe_tree_seed_default(T);
    cl = strlen(T->catalog_dir);
    if (cl + 18 >= sizeof path) return -5;
    memcpy(path, T->catalog_dir, cl);
    memcpy(path + cl, "/skill_tree.jsonl", 18);
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char id[ROE_TREE_NAME];
        int rank = 0;
        const char *p;
        RoeTreeNode *node;
        id[0] = 0;
        p = strstr(line, "\"id\":\"");
        if (p) sscanf(p, "\"id\":\"%63[^\"]\"", id);
        p = strstr(line, "\"rank\":");
        if (p) sscanf(p, "\"rank\":%d", &rank);
        node = find_mut(T, id);
        if (node && rank > 0) {
            if (rank > node->max_rank) rank = node->max_rank;
            node->rank = rank;
            n++;
        }
    }
    fclose(f);
    return n;
}

void roe_tree_dump(const RoeTree *T, char *buf, size_t cap) {
    RoeTreeSnapshot S;
    if (!buf || !cap) return;
    roe_tree_snapshot(T, &S);
    snprintf(buf, cap, "%s unlock_ok=%llu blocked=%llu peak=%.1f", S.summary,
             T ? (unsigned long long)T->n_unlock_ok : 0ULL,
             T ? (unsigned long long)T->n_unlock_blocked : 0ULL,
             T ? T->peak_power : 0.0);
}
