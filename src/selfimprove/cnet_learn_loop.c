#include "../../include/cnet_learn_loop.h"
#include "../../include/cnet_math_solve.h"
#include "../../include/agent_memory.h"
#include "../../include/contract/mcp_wiki.h"
#include "../../include/contract/mcp_web_search.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define CNET_MKDIR(p) _mkdir(p)
#else
#include <sys/types.h>
#include <errno.h>
#define CNET_MKDIR(p) mkdir((p), 0755)
#endif

static void trim_inplace(char *s) {
    size_t n, i = 0, j;
    if (!s) return;
    n = strlen(s);
    while (i < n && isspace((unsigned char)s[i])) i++;
    j = n;
    while (j > i && isspace((unsigned char)s[j - 1])) j--;
    if (i > 0 || j < n) {
        memmove(s, s + i, j - i);
        s[j - i] = '\0';
    }
}

static int useful_text(const char *s) {
    size_t n;
    if (!s || !s[0]) return 0;
    if (strcmp(s, "LOOKUP_FAILED") == 0) return 0;
    if (strncmp(s, "No useful", 9) == 0) return 0;
    if (strncmp(s, "Web search failed", 17) == 0) return 0;
    /* Reject agent-memory self-noise (thoughts recorded by this loop). */
    if (strstr(s, "cnet_learn_loop") != NULL) return 0;
    if (strstr(s, "tool miss") != NULL) return 0;
    if (strstr(s, "One-shot build") != NULL) return 0;
    if (strstr(s, "answered from procedural") != NULL) return 0;
    if (strstr(s, "Could not find a useful") != NULL) return 0;
    n = strlen(s);
    /* Short pure answers (math): "5", "yes, 17 is prime", "3 and 2" */
    if (n >= 1 && n < 40) {
        int digits = 0, i;
        for (i = 0; s[i]; i++)
            if (isdigit((unsigned char)s[i])) digits++;
        if (digits > 0) return 1;
        if (strncmp(s, "yes", 3) == 0 || strncmp(s, "no", 2) == 0) return 1;
    }
    return n >= 40; /* encyclopedia prose */
}

/* Strip question phrasing so Wikipedia titles resolve (e.g. "What is malloc in C" â†’ "malloc"). */
static void wiki_topic_from_query(const char *query, char *out, size_t cap) {
    char tmp[256];
    char *tok;
    const char *skip[] = {
        "what", "is", "are", "a", "an", "the", "in", "of", "for", "how", "does",
        "do", "to", "on", "about", "tell", "me", "please", "define", "explain",
        NULL
    };
    size_t o = 0;
    int first = 1;
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!query) return;
    snprintf(tmp, sizeof tmp, "%s", query);
    for (tok = strtok(tmp, " \t?.,!\"'"); tok; tok = strtok(NULL, " \t?.,!\"'")) {
        int sk = 0, i;
        char lower[64];
        size_t k, tl = strlen(tok);
        if (tl == 0 || tl >= sizeof lower) continue;
        for (k = 0; k < tl; k++) lower[k] = (char)tolower((unsigned char)tok[k]);
        lower[tl] = '\0';
        for (i = 0; skip[i]; i++)
            if (strcmp(lower, skip[i]) == 0) { sk = 1; break; }
        if (sk) continue;
        if (!first && o + 1 < cap) out[o++] = ' ';
        first = 0;
        for (k = 0; k < tl && o + 1 < cap; k++) out[o++] = tok[k];
    }
    out[o] = '\0';
    if (o < 2) snprintf(out, cap, "%s", query);
}

static void slugify(const char *in, char *out, size_t cap) {
    size_t i = 0, o = 0;
    int prev_dash = 1;
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!in) return;
    for (i = 0; in[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c)) {
            out[o++] = (char)tolower(c);
            prev_dash = 0;
        } else if (!prev_dash && (c == ' ' || c == '-' || c == '_' || c == '/')) {
            out[o++] = '-';
            prev_dash = 1;
        }
    }
    while (o > 0 && out[o - 1] == '-') o--;
    out[o] = '\0';
    if (o == 0) {
        snprintf(out, cap, "skill");
    }
    if (o > 48) out[48] = '\0';
}

static int mkdir_p(const char *path) {
    char tmp[512];
    size_t len, i;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (CNET_MKDIR(tmp) != 0 && errno != EEXIST) {
                /* continue; parent may exist */
            }
            tmp[i] = '/';
        }
    }
    if (CNET_MKDIR(tmp) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int path_is_dir(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int window_width(const char *window_path) {
    FILE *f;
    char line[64];
    int n = 0;
    if (!window_path || !window_path[0]) return 256;
    f = fopen(window_path, "r");
    if (!f) return 256;
    while (fgets(line, sizeof line, f)) {
        if (line[0] >= '0' && line[0] <= '9') n++;
    }
    fclose(f);
    return n > 0 ? n : 256;
}

static int seed_teachable_gap(const CnetLearnConfig *cfg, int token_id) {
    FILE *f;
    int W, K;
    if (!cfg || !cfg->inbox_path[0]) return -1;
    W = window_width(cfg->window_path);
    K = cfg->gap_k > 0 ? cfg->gap_k : 3;
    f = fopen(cfg->inbox_path, "a");
    if (!f) return -1;
    fprintf(f, "NO_PLAN 1 %d 1 w_cur 1 %d %d tk%dq%d\n", W, W, K, token_id, token_id);
    fclose(f);
    return 0;
}

/* First numeric token-ish seed from query length hash into window range â€” weak but
 * keeps gap seeding lane-shaped without needing a full tokenizer. */
static int pseudo_token_from_query(const char *q) {
    unsigned long h = 2166136261u;
    size_t i;
    if (!q) return 506;
    for (i = 0; q[i]; i++) {
        h ^= (unsigned char)q[i];
        h *= 16777619u;
    }
    return (int)(500 + (h % 1500));
}

static int try_read_skill(const char *skills_dir, const char *slug,
                          char *answer, size_t answer_cap, char *path_out, size_t path_cap) {
    char path[512];
    FILE *f;
    char line[512];
    int in_answer = 0;
    size_t used = 0;
    if (!skills_dir || !slug || !answer || answer_cap == 0) return 0;
    snprintf(path, sizeof path, "%s/%s/SKILL.md", skills_dir, slug);
    f = fopen(path, "r");
    if (!f) return 0;
    answer[0] = '\0';
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "## Answer", 9) == 0) {
            in_answer = 1;
            continue;
        }
        if (in_answer && line[0] == '#' && line[1] == '#') break;
        if (in_answer) {
            size_t ln = strlen(line);
            if (used + ln + 1 < answer_cap) {
                memcpy(answer + used, line, ln);
                used += ln;
                answer[used] = '\0';
            }
        }
    }
    fclose(f);
    trim_inplace(answer);
    if (!useful_text(answer)) return 0;
    if (path_out && path_cap)
        snprintf(path_out, path_cap, "%s", path);
    return 1;
}

static int write_skill_md(const char *skills_dir, const char *slug, const char *name,
                          const char *query, const char *answer, const char *source_name,
                          char *path_out, size_t path_cap) {
    char dir[512], path[512];
    FILE *f;
    time_t now = time(NULL);
    struct tm tm_buf;
    char ts[64];
    if (!skills_dir || !slug || !answer) return -1;
    snprintf(dir, sizeof dir, "%s/%s", skills_dir, slug);
    if (mkdir_p(dir) != 0) return -1;
    snprintf(path, sizeof path, "%s/SKILL.md", dir);
    f = fopen(path, "w");
    if (!f) return -1;
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm_buf);
    fprintf(f,
            "---\n"
            "name: %s\n"
            "description: Learned procedure for query '%s'\n"
            "source: %s\n"
            "learned_at: %s\n"
            "origin: cnet_learn_loop\n"
            "---\n\n"
            "# %s\n\n"
            "## When to use\n"
            "User asks something like: %s\n\n"
            "## Procedure (Hermes-style procedural memory in C)\n"
            "1. Check declarative memory (MCP facts / agent KB).\n"
            "2. On miss, call wiki_lookup then web_search.\n"
            "3. Memorize successful result under the query key.\n"
            "4. Reuse this skill on similar queries.\n\n"
            "## Answer\n"
            "%s\n",
            name ? name : slug, query ? query : "", source_name ? source_name : "unknown",
            ts, name ? name : slug, query ? query : "", answer);
    fclose(f);
    if (path_out && path_cap) snprintf(path_out, path_cap, "%s", path);
    return 0;
}

void cnet_learn_config_defaults(CnetLearnConfig *c) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->use_tools = 1;
    c->write_skill = 1;
    c->memorize = 1;
    c->seed_gap = 0;
    c->prefer_wiki = 1;
    c->gap_k = 3;
    snprintf(c->skills_dir, sizeof c->skills_dir, "logs/personal_ai_skills");
    c->inbox_path[0] = '\0';
    c->window_path[0] = '\0';
}

void cnet_learn_config_from_env(CnetLearnConfig *c) {
    const char *e;
    if (!c) return;
    cnet_learn_config_defaults(c);
    e = getenv("CNET_SKILLS_DIR");
    if (e && e[0]) snprintf(c->skills_dir, sizeof c->skills_dir, "%s", e);
    e = getenv("CNET_GAP_INBOX");
    if (e && e[0]) snprintf(c->inbox_path, sizeof c->inbox_path, "%s", e);
    e = getenv("CNET_WINDOW_FILE");
    if (!e || !e[0]) e = getenv("CNET_RESIDUAL_WINDOW");
    if (e && e[0]) snprintf(c->window_path, sizeof c->window_path, "%s", e);
    e = getenv("CNET_LEARN_SEED_GAP");
    if (e && e[0] == '1') c->seed_gap = 1;
    e = getenv("CNET_LEARN_WRITE_SKILL");
    if (e && e[0] == '0') c->write_skill = 0;
    e = getenv("CNET_LEARN_USE_TOOLS");
    if (e && e[0] == '0') c->use_tools = 0;
}

const char *cnet_learn_source_name(CnetLearnSource s) {
    switch (s) {
    case CNET_LEARN_SRC_MEMORY: return "memory";
    case CNET_LEARN_SRC_WIKI: return "wiki";
    case CNET_LEARN_SRC_WEB: return "web";
    case CNET_LEARN_SRC_SKILL: return "skill";
    default: return "none";
    }
}

int cnet_learn_list_skills(const char *skills_dir, char names[][128], int max_n) {
    /* Portable enough: skills_dir listing via popen ls (Linux personal-ai host). */
    char cmd[640];
    FILE *p;
    char line[256];
    int n = 0;
    if (!skills_dir || !names || max_n <= 0) return 0;
    if (!path_is_dir(skills_dir)) return 0;
    snprintf(cmd, sizeof cmd, "ls -1 %s 2>/dev/null", skills_dir);
    p = popen(cmd, "r");
    if (!p) return 0;
    while (n < max_n && fgets(line, sizeof line, p)) {
        trim_inplace(line);
        if (!line[0] || line[0] == '.') continue;
        snprintf(names[n], 128, "%s", line);
        n++;
    }
    pclose(p);
    return n;
}

int cnet_learn_cycle(const char *query, const CnetLearnConfig *cfg_in, CnetLearnReport *rep) {
    CnetLearnConfig local, *cfg;
    char slug[128];
    char buf[1024];
    char agent_buf[1024];
    int from_cache = 0;
    int rc;

    if (rep) memset(rep, 0, sizeof *rep);
    if (!query || !query[0]) return -1;

    if (cfg_in) {
        local = *cfg_in;
        cfg = &local;
    } else {
        cnet_learn_config_from_env(&local);
        cfg = &local;
    }
    if (!cfg->skills_dir[0])
        snprintf(cfg->skills_dir, sizeof cfg->skills_dir, "logs/personal_ai_skills");

    mcp_memory_init();
    agent_memory_init();
    agent_record_user(query);

    slugify(query, slug, sizeof slug);
    if (rep) snprintf(rep->skill_name, sizeof rep->skill_name, "%s", slug);

    /* -1) Math solve first (creative plan â†’ math_eval â†’ verify). */
    {
        CnetMathSolveConfig mc;
        CnetMathSolveReport mr;
        cnet_math_solve_config_from_env(&mc);
        snprintf(mc.skills_dir, sizeof mc.skills_dir, "%s", cfg->skills_dir);
        mc.write_skill = cfg->write_skill;
        mc.memorize = cfg->memorize;
        if (cnet_math_solve(query, &mc, &mr) == 0 && mr.verified && useful_text(mr.answer)) {
            if (rep) {
                rep->source = CNET_LEARN_SRC_SKILL;
                rep->memorized = mr.skill_written ? 0 : 1;
                rep->skill_written = mr.skill_written;
                snprintf(rep->answer, sizeof rep->answer, "%s", mr.answer);
                snprintf(rep->skill_path, sizeof rep->skill_path, "%s", mr.skill_path);
                snprintf(rep->detail, sizeof rep->detail, "math_solve %s method=%s steps=%s",
                         cnet_math_tier_name(mr.tier), mr.method, mr.steps);
            }
            agent_record_assistant(mr.answer);
            agent_record_thought("cnet_learn_loop: math_solve verified");
            agent_save_session();
            return 0;
        }
    }

    /* 0) Reuse existing procedural skill (Hermes skill_view equivalent). */
    if (!cfg->force_tools &&
        try_read_skill(cfg->skills_dir, slug, buf, sizeof buf,
                       rep ? rep->skill_path : NULL,
                       rep ? sizeof rep->skill_path : 0)) {
        if (rep) {
            rep->source = CNET_LEARN_SRC_SKILL;
            rep->skill_reused = 1;
            snprintf(rep->answer, sizeof rep->answer, "%s", buf);
            snprintf(rep->detail, sizeof rep->detail, "reused skill %s", slug);
        }
        agent_record_assistant(buf);
        agent_record_thought("cnet_learn_loop: answered from procedural skill");
        agent_save_session();
        return 0;
    }

    /* 1) Declarative memory (MCP facts). */
    buf[0] = '\0';
    if (!cfg->force_tools &&
        mcp_recall_fact(query, buf, sizeof buf) && useful_text(buf)) {
        if (rep) {
            rep->source = CNET_LEARN_SRC_MEMORY;
            rep->from_cache = 1;
            snprintf(rep->answer, sizeof rep->answer, "%s", buf);
            snprintf(rep->detail, sizeof rep->detail, "mcp fact memory hit");
        }
        /* Hermes: promote known fact into procedural skill if missing. */
        if (cfg->write_skill) {
            if (write_skill_md(cfg->skills_dir, slug, slug, query, buf, "memory",
                               rep ? rep->skill_path : NULL,
                               rep ? sizeof rep->skill_path : 0) == 0) {
                if (rep) rep->skill_written = 1;
            }
        }
        agent_record_assistant(buf);
        agent_save_session();
        return 0;
    }

    /* 1b) Agent knowledge recall (returns found count). */
    agent_buf[0] = '\0';
    if (!cfg->force_tools &&
        agent_recall_knowledge(query, agent_buf, sizeof agent_buf, 2) > 0 &&
        useful_text(agent_buf)) {
        if (rep) {
            rep->source = CNET_LEARN_SRC_MEMORY;
            snprintf(rep->answer, sizeof rep->answer, "%s", agent_buf);
            snprintf(rep->detail, sizeof rep->detail, "agent KB hit");
        }
        if (cfg->write_skill) {
            if (write_skill_md(cfg->skills_dir, slug, slug, query, agent_buf, "memory",
                               rep ? rep->skill_path : NULL,
                               rep ? sizeof rep->skill_path : 0) == 0) {
                if (rep) rep->skill_written = 1;
            }
        }
        agent_record_assistant(agent_buf);
        agent_save_session();
        return 0;
    }

    if (!cfg->use_tools) {
        if (rep) {
            rep->source = CNET_LEARN_SRC_NONE;
            snprintf(rep->detail, sizeof rep->detail, "miss and tools disabled");
        }
        agent_record_assistant("I do not know yet (tools disabled).");
        agent_save_session();
        return 1;
    }

    /* 2) Tools: wiki then web (Hermes tool use).
     * Use stripped topic for wiki ("malloc" not "What is malloc in C"). */
    {
        char topic[256];
        wiki_topic_from_query(query, topic, sizeof topic);

        buf[0] = '\0';
        from_cache = 0;
        if (cfg->prefer_wiki) {
            rc = port_contract_mcp_wiki_lookup(topic, buf, sizeof buf, &from_cache);
            if (rc != 0 || !useful_text(buf))
                rc = port_contract_mcp_wiki_lookup(query, buf, sizeof buf, &from_cache);
            if (rc == 0 && useful_text(buf)) {
                if (rep) {
                    rep->source = CNET_LEARN_SRC_WIKI;
                    rep->from_cache = from_cache;
                }
                goto learned;
            }
        }
        rc = port_contract_mcp_web_search(topic[0] ? topic : query, buf, sizeof buf,
                                         &from_cache);
        if (rc != 0 || !useful_text(buf))
            rc = port_contract_mcp_web_search(query, buf, sizeof buf, &from_cache);
        if (rc == 0 && useful_text(buf)) {
            if (rep) {
                rep->source = CNET_LEARN_SRC_WEB;
                rep->from_cache = from_cache;
            }
            goto learned;
        }
        if (!cfg->prefer_wiki) {
            rc = port_contract_mcp_wiki_lookup(topic, buf, sizeof buf, &from_cache);
            if (rc == 0 && useful_text(buf)) {
                if (rep) {
                    rep->source = CNET_LEARN_SRC_WIKI;
                    rep->from_cache = from_cache;
                }
                goto learned;
            }
        }
    }

    if (rep) {
        rep->source = CNET_LEARN_SRC_NONE;
        snprintf(rep->answer, sizeof rep->answer, "%s",
                 buf[0] ? buf : "LOOKUP_FAILED");
        snprintf(rep->detail, sizeof rep->detail, "tools returned nothing useful");
    }
    agent_record_assistant("Could not find a useful answer yet.");
    agent_record_thought("cnet_learn_loop: tool miss");
    agent_save_session();
    return 1;

learned:
    if (rep) snprintf(rep->answer, sizeof rep->answer, "%s", buf);

    /* 3) Declarative memory write (Hermes memory add). */
    if (cfg->memorize) {
        mcp_memorize_fact(query, buf);
        if (rep) rep->memorized = 1;
        agent_record_thought("cnet_learn_loop: memorized fact");
    }

    /* 4) Procedural skill write (Hermes skill_manage create). */
    if (cfg->write_skill) {
        const char *src = rep ? cnet_learn_source_name(rep->source) : "tool";
        if (write_skill_md(cfg->skills_dir, slug, slug, query, buf, src,
                           rep ? rep->skill_path : NULL,
                           rep ? sizeof rep->skill_path : 0) == 0) {
            if (rep) rep->skill_written = 1;
            agent_record_thought("cnet_learn_loop: wrote procedural skill");
        }
    }

    /* 5) Optional certified-path demand seed (CNET-specific). */
    if (cfg->seed_gap && cfg->inbox_path[0]) {
        if (seed_teachable_gap(cfg, pseudo_token_from_query(query)) == 0) {
            if (rep) rep->gap_seeded = 1;
        }
    }

    agent_record_assistant(buf);
    agent_save_session();
    if (rep)
        snprintf(rep->detail, sizeof rep->detail, "learned via %s mem=%d skill=%d gap=%d",
                 cnet_learn_source_name(rep->source), rep->memorized, rep->skill_written,
                 rep->gap_seeded);
    return 0;
}
