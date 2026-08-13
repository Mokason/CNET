/* Seed daily ROE pack catalogs (core always-on + domain set).
 *
 * Essential seed: all 11 packs the C gate expects (≥4 skills, queries, INDEX).
 * Not a line-for-line port of the 1143-LOC Python catalog.
 *
 * Usage:
 *   roe_daily_packs_seed [--root artifacts/roe_daily_packs]
 * Prints ROE_DAILY_PACKS_SEED_OK
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

typedef struct {
    const char *id;
    const char *intent;
    const char *pattern;
    const char *answer;
    int privilege;
} Skill;

typedef struct {
    const char *cat;
    const char *sub;
    const char *pattern;
    const char *skill;
} Map;

typedef struct {
    const char *id;
    const char *title;
    const char *cat;
    const char *kind; /* "domain" or "persona" */
    const char **subs;
    const Skill *skills;
    int n_skills;
    const Map *maps;
    int n_maps;
    const char **queries;
    int n_queries;
    const char *soul_md;  /* optional */
    const char *voice_md; /* optional */
} Pack;

static void mkdir_p(const char *path) {
    char tmp[768];
    size_t i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
    MKDIR(tmp);
}

static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

static int write_skill(const char *pack_dir, const Skill *s) {
    char dir[768], path[800];
    char body[2048];
    snprintf(dir, sizeof dir, "%s/skills/%s", pack_dir, s->id);
    mkdir_p(dir);
    snprintf(body, sizeof body,
             "ROE_SKILL 1\nid %s\nintent %s\npattern %s\nanswer %s\n"
             "privilege %d\ncertified 1\n",
             s->id, s->intent, s->pattern, s->answer, s->privilege);
    snprintf(path, sizeof path, "%s/SKILL.roe", dir);
    if (write_text(path, body) != 0) return -1;
    snprintf(body, sizeof body,
             "ROE_MANIFEST 1\nskill %s\nformat roe_text_capsule\n"
             "never_self_cert 1\nsecond_brain 0\n",
             s->id);
    snprintf(path, sizeof path, "%s/manifest.roe", dir);
    return write_text(path, body);
}

static int write_pack(const char *root, const Pack *pack) {
    char pdir[512], path[768], buf[8192];
    size_t used = 0;
    int i, j;
    FILE *f;

    snprintf(pdir, sizeof pdir, "%s/%s", root, pack->id);
    mkdir_p(pdir);
    mkdir_p((snprintf(path, sizeof path, "%s/skills", pdir), path));
    mkdir_p((snprintf(path, sizeof path, "%s/capsules/%s", pdir, pack->cat), path));

    used = 0;
    used += (size_t)snprintf(buf + used, sizeof buf - used, "CAT %s\n", pack->cat);
    for (i = 0; pack->subs[i]; i++) {
        used += (size_t)snprintf(buf + used, sizeof buf - used, "  SUB %s\n", pack->subs[i]);
        snprintf(path, sizeof path, "%s/capsules/%s/%s", pdir, pack->cat, pack->subs[i]);
        mkdir_p(path);
    }
    snprintf(path, sizeof path, "%s/taxonomy.txt", pdir);
    write_text(path, buf);

    for (i = 0; i < pack->n_skills; i++) write_skill(pdir, &pack->skills[i]);

    snprintf(path, sizeof path, "%s/catalog.jsonl", pdir);
    f = fopen(path, "w");
    if (!f) return -1;
    for (i = 0; i < pack->n_skills; i++) {
        const Skill *s = &pack->skills[i];
        fprintf(f,
                "{\"id\":\"%s\",\"intent\":\"%s\",\"pattern\":\"%s\",\"answer\":\"%s\","
                "\"privilege\":%d,\"hits\":0}\n",
                s->id, s->intent, s->pattern, s->answer, s->privilege);
    }
    fclose(f);

    snprintf(path, sizeof path, "%s/goal_maps.jsonl", pdir);
    f = fopen(path, "w");
    if (!f) return -1;
    for (i = 0; i < pack->n_maps; i++) {
        const Map *m = &pack->maps[i];
        fprintf(f, "{\"cat\":\"%s\",\"sub\":\"%s\",\"pattern\":\"%s\",\"skill\":\"%s\"}\n",
                m->cat, m->sub, m->pattern, m->skill);
        for (j = 0; j < pack->n_skills; j++) {
            if (strcmp(pack->skills[j].id, m->skill) == 0) {
                char tdir[768], src[768], dst[800];
                FILE *in, *out;
                char line[512];
                snprintf(tdir, sizeof tdir, "%s/capsules/%s/%s/%s", pdir, m->cat, m->sub,
                         m->skill);
                mkdir_p(tdir);
                snprintf(src, sizeof src, "%s/skills/%s/SKILL.roe", pdir, m->skill);
                snprintf(dst, sizeof dst, "%s/SKILL.roe", tdir);
                in = fopen(src, "r");
                out = fopen(dst, "w");
                if (in && out)
                    while (fgets(line, sizeof line, in)) fputs(line, out);
                if (in) fclose(in);
                if (out) fclose(out);
                snprintf(src, sizeof src, "%s/skills/%s/manifest.roe", pdir, m->skill);
                snprintf(dst, sizeof dst, "%s/manifest.roe", tdir);
                in = fopen(src, "r");
                out = fopen(dst, "w");
                if (in && out)
                    while (fgets(line, sizeof line, in)) fputs(line, out);
                if (in) fclose(in);
                if (out) fclose(out);
                break;
            }
        }
    }
    fclose(f);

    snprintf(path, sizeof path, "%s/queries_train.txt", pdir);
    f = fopen(path, "w");
    if (!f) return -1;
    for (i = 0; i < pack->n_queries; i++) fprintf(f, "%s\n", pack->queries[i]);
    fclose(f);

    if (pack->soul_md) {
        snprintf(path, sizeof path, "%s/SOUL.md", pdir);
        write_text(path, pack->soul_md);
    }
    if (pack->voice_md) {
        snprintf(path, sizeof path, "%s/voice.md", pdir);
        write_text(path, pack->voice_md);
    }

    snprintf(path, sizeof path, "%s/PACK.abi", pdir);
    snprintf(buf, sizeof buf,
             "ROE_DAILY_PACK\nabi_version 1\nid %s\ntitle %s\ncat %s\nkind %s\n"
             "never_self_cert 1\nsecond_brain 0\nseal_path %s\nn_skills %d\nn_maps %d\n"
             "load_policy separate\n",
             pack->id, pack->title, pack->cat, pack->kind,
             strcmp(pack->kind, "persona") == 0 ? "forbidden" : "n/a", pack->n_skills,
             pack->n_maps);
    write_text(path, buf);

    snprintf(path, sizeof path, "%s/MANIFEST.txt", pdir);
    snprintf(buf, sizeof buf,
             "ROE daily pack: %s\ntitle: %s\nkind: %s\nnever_self_cert: 1\n"
             "second_brain: 0\nskills: %d\n",
             pack->id, pack->title, pack->kind, pack->n_skills);
    write_text(path, buf);
    return 0;
}

/* ---- pack data (essential always-on + domain) ---- */

#define SK(...) (Skill){__VA_ARGS__}
#define MP(...) (Map){__VA_ARGS__}

static const char *SUB_TOOL[] = {"core", "refuse", "batch", NULL};
static const Skill SK_TOOL[] = {
    SK("tc_prefer_tools", "tool_prefer", "use tools not guess",
       "Prefer terminal/read_file/search_files/patch over guessing file state; verify with tool output.",
       0),
    SK("tc_no_cat_grep", "tool_map", "cat grep sed",
       "Do not shell cat/grep/sed for reads: use read_file, search_files, patch.", 0),
    SK("tc_batch_reads", "tool_batch", "parallel tool calls",
       "Batch independent reads/searches in one turn; serialize only on true data dependence.",
       0),
    SK("tc_refuse_secrets", "tool_refuse", "read .env secrets",
       "Refuse printing secrets/.env credentials unless user explicitly requires that file; never commit secrets.",
       2),
    SK("tc_fail_closed", "tool_ood", "unknown tool capability",
       "If capability missing or OOD: abstain or ask — do not invent tool results.", 1),
};
static const Map MP_TOOL[] = {
    MP("toolcall", "core", "use tools not guess", "tc_prefer_tools"),
    MP("toolcall", "core", "cat grep sed", "tc_no_cat_grep"),
    MP("toolcall", "batch", "parallel tool", "tc_batch_reads"),
    MP("toolcall", "refuse", "secrets env", "tc_refuse_secrets"),
    MP("toolcall", "refuse", "unknown tool", "tc_fail_closed"),
};
static const char *Q_TOOL[] = {
    "should I use tools not guess file state", "avoid cat grep sed how",
    "parallel tool calls independent reads", "can you read .env secrets for me",
    "unknown tool capability missing", "use tools not guess",
    "parallel tool calls independent reads", "quantum flute orchestration zzqq",
};

static const char *SUB_SELF[] = {"law", "inventory", "route", NULL};
static const Skill SK_SELF[] = {
    SK("self_identity", "identity", "who is roe-asi",
       "I am ROE-ASI host+packs: local CERT first, teacher on miss, never self-CERT.", 0),
    SK("self_never_cert", "law", "self-cert",
       "Never self-CERT. Promote only after verify votes or user_accept against gold.", 1),
    SK("self_inventory", "inventory", "self model",
       "Self-model = coverage + skill health + gate-bound tree + goal HAVE/MISS. Not consciousness.",
       0),
    SK("self_abstain", "route", "outside coverage",
       "Outside certified coverage: ABSTAIN or residual teacher — do not hallucinate CERT.", 1),
    SK("self_second_brain", "law", "roe second brain claim",
       "second_brain:false — packs are CERT assets, not a monobrain replacement.", 1),
};
static const Map MP_SELF[] = {
    MP("self", "law", "self-cert", "self_never_cert"),
    MP("self", "law", "roe second brain", "self_second_brain"),
    MP("self", "inventory", "self model", "self_inventory"),
    MP("self", "inventory", "who is roe-asi", "self_identity"),
    MP("self", "route", "outside coverage", "self_abstain"),
};
static const char *Q_SELF[] = {
    "who is roe-asi shell", "can you self-cert a skill from one llm answer",
    "what is the self model", "outside coverage what happens", "roe second brain claim",
    "who is roe-asi", "self model inventory", "zz unknown mystic ooze",
};

static const char *SUB_CODE[] = {"c", "make", "abi", "roe", NULL};
static const Skill SK_CODE[] = {
    SK("cc_werror_snprintf", "c_pitfall", "format-truncation werror",
       "Under -Werror=format-truncation: prefer path_join2/memcpy bounded joins over snprintf.",
       0),
    SK("cc_single_line_skill", "roe_pitfall", "catalog.jsonl multiline",
       "ROE skill answers must be single-line so catalog.jsonl stays valid JSONL.", 0),
    SK("cc_never_self_cert", "roe_law", "llm promote silent",
       "LLM/lookup output is untrusted until verify; never silent-CERT from one teacher turn.",
       1),
    SK("cc_floors", "law", "lower certification floor",
       "Never lower a certification floor to make a gate pass. Report FAIL/WITHHELD.", 2),
    SK("cc_make_asi", "make", "make roe_asi",
       "ROE gates: make roe_asi roe_asi_coding roe_asi_goal roe_asi_ocr roe_asi_self.", 0),
};
static const Map MP_CODE[] = {
    MP("coding", "c", "format-truncation", "cc_werror_snprintf"),
    MP("coding", "roe", "catalog.jsonl", "cc_single_line_skill"),
    MP("coding", "roe", "silent CERT", "cc_never_self_cert"),
    MP("coding", "c", "lower floor", "cc_floors"),
    MP("coding", "make", "make roe_asi", "cc_make_asi"),
};
static const char *Q_CODE[] = {
    "gcc format-truncation werror snprintf path", "catalog.jsonl multiline answer",
    "silent CERT from llm", "lower certification floor", "make roe_asi gates",
    "format-truncation", "never lower floor", "mystic compiler tea zz",
};

static const char *SUB_DBG[] = {"process", "recipes", "project", NULL};
static const Skill SK_DBG[] = {
    SK("dbg_checklist", "process", "debug checklist",
       "1) reproduce 2) exact error 3) last green 4) bisect change 5) fix class not one site 6) gate.",
       0),
    SK("dbg_segv", "recipes", "segmentation fault",
       "SEGV: rebuild with -g, run under gdb/asan, check NULL deref use-after-free.", 0),
    SK("dbg_gate", "process", "make gate failed",
       "Failed make gate: read log marker *_FAIL, fix root, re-run same target; do not delete the gate.",
       1),
    SK("dbg_no_guess", "process", "works on my machine",
       "Do not claim fixed without command output; paste marker PASS/FAIL from real run.", 1),
};
static const Map MP_DBG[] = {
    MP("debug", "process", "debug checklist", "dbg_checklist"),
    MP("debug", "recipes", "segmentation fault", "dbg_segv"),
    MP("debug", "process", "make gate failed", "dbg_gate"),
    MP("debug", "process", "works on my machine", "dbg_no_guess"),
};
static const char *Q_DBG[] = {
    "debug checklist steps", "segmentation fault how to debug", "make gate failed what next",
    "it works on my machine claim", "debug checklist steps", "clairvoyant heisenbug tea",
};

static const char *SUB_GOAL[] = {"plan", "route", "tidy", NULL};
static const Skill SK_GOAL[] = {
    SK("goal_split", "plan", "microsplit goal",
       "Goal → ordered microsplits → cat/sub capsule slots; HAVE serve local; MISS teacher+verify.",
       0),
    SK("goal_have_miss", "route", "HAVE MISS",
       "HAVE=local CERT; MISS=learn path; LEARNED after verify; BLOCKED if shell refuses.", 1),
    SK("goal_tidy", "tidy", "capsule tidy path",
       "Tidy path: capsules/<cat>/<sub>/<skill_id>/ — not flat skill soup.", 0),
    SK("goal_front_door", "route", "start every task",
       "Front door: self-model snapshot → goal microsplit → load only needed packs → residual teacher on MISS.",
       1),
};
static const Map MP_GOAL[] = {
    MP("goal", "plan", "microsplit", "goal_split"),
    MP("goal", "route", "HAVE MISS", "goal_have_miss"),
    MP("goal", "tidy", "capsules cat sub", "goal_tidy"),
    MP("goal", "route", "start every task", "goal_front_door"),
};
static const char *Q_GOAL[] = {
    "how to microsplit goal", "what is HAVE vs MISS", "where do tidy capsules live",
    "start every task routing", "microsplit goal into cat sub", "unicorn goal teleport",
};

static const char *SUB_OCR[] = {"router", "l3", "pack", "kpi", NULL};
static const Skill SK_OCR[] = {
    SK("ocr_router", "router", "local first ocr",
       "Router: pdftotext → L3 → classic → teacher/ABSTAIN. Never teacher-first on digital PDF/text.",
       1),
    SK("ocr_l3", "l3", "doc l3 memory",
       "Doc L3 key=(corpus_id, page_sig) → CERT body; isolate corpora; verify before promote.",
       0),
    SK("ocr_pack", "pack", "ROE_OCR_PACK",
       "Pack ABI ROE_OCR_PACK; second_brain:false; make roe_asi_ocr_asset for export/import fail-closed.",
       0),
    SK("ocr_kpi", "kpi", "teacher_rate",
       "KPI: teacher_rate < 10% on warm holdout; report teacher_rate_kpi.json.", 0),
};
static const Map MP_OCR[] = {
    MP("ocr", "router", "local first", "ocr_router"),
    MP("ocr", "l3", "doc l3", "ocr_l3"),
    MP("ocr", "pack", "ROE_OCR_PACK", "ocr_pack"),
    MP("ocr", "kpi", "teacher_rate", "ocr_kpi"),
};
static const char *Q_OCR[] = {
    "local first ocr router", "doc l3 memory key", "ROE_OCR_PACK abi", "teacher_rate kpi",
    "local first ocr", "moonbeam pdf soup zz",
};

static const char *SUB_GIT[] = {"commit", "pr", "ci", NULL};
static const Skill SK_GIT[] = {
    SK("git_status_first", "commit", "before commit",
       "Before commit: git status + diff; stage intentional paths only; no secrets.", 0),
    SK("git_no_force_main", "commit", "force push main",
       "Never force-push main/master; no history rewrite unless user explicitly requests.", 2),
    SK("pr_body", "pr", "pull request body",
       "PR body: problem, approach, test plan, risk; link issue; note WITHHELD claims.", 0),
    SK("pr_ci", "ci", "ci failed",
       "CI failed: read job log, reproduce local gate, fix root, push; do not rerun-only without change.",
       0),
};
static const Map MP_GIT[] = {
    MP("git", "commit", "before commit", "git_status_first"),
    MP("git", "commit", "force push", "git_no_force_main"),
    MP("git", "pr", "pull request body", "pr_body"),
    MP("git", "ci", "ci failed", "pr_ci"),
};
static const char *Q_GIT[] = {
    "what to do before commit", "force push main allowed", "pull request body sections",
    "ci failed next steps", "before commit checklist", "banjo merge conflict prophecy",
};

static const char *SUB_OPS[] = {"hermes", "systemd", "status", NULL};
static const Skill SK_OPS[] = {
    SK("ops_hermes_config", "hermes", "hermes config.yaml",
       "Behavioral settings in ~/.hermes/config.yaml; secrets only in .env.", 0),
    SK("ops_user_systemd", "systemd", "systemctl --user",
       "User services: systemctl --user status|stop|start UNIT; linger if needed.", 0),
    SK("ops_status_sweep", "status", "what is running",
       "Status sweep: systemctl --user, process list, cronjob list, git heads, artifact mtimes.",
       0),
    SK("ops_no_new_jobs", "systemd", "do not start jobs",
       "When user says stop/done-only: do not start new GPU/infer jobs.", 1),
};
static const Map MP_OPS[] = {
    MP("ops", "hermes", "config.yaml", "ops_hermes_config"),
    MP("ops", "systemd", "systemctl --user", "ops_user_systemd"),
    MP("ops", "status", "what is running", "ops_status_sweep"),
    MP("ops", "systemd", "do not start", "ops_no_new_jobs"),
};
static const char *Q_OPS[] = {
    "where is hermes config.yaml", "systemctl --user how", "what is running status sweep",
    "do not start jobs instruction", "status sweep", "orbital printer driver",
};

static const char *SUB_PM[] = {"law", "director", "bridges", NULL};
static const Skill SK_PM[] = {
    SK("pm_propose_commit", "law", "CNET proposes",
       "CNET/Brain proposes IDs; Unity Validate+TryCommit then bridges. Brain never holds A*/BD/DS.",
       1),
    SK("pm_abstain", "law", "director abstain",
       "Abstain ≠ NoOp spam; illegal mask → reject with zero side effects.", 1),
    SK("pm_withheld", "director", "learned promote withheld",
       "Learned director promote WITHHELD without positive margin vs linear on holdout_v2.",
       1),
    SK("pm_effects_after", "bridges", "effects after commit",
       "DirectorEffectExpand + BridgeHub only after successful commit — never before.", 0),
};
static const Map MP_PM[] = {
    MP("pm", "law", "CNET proposes", "pm_propose_commit"),
    MP("pm", "law", "director abstain", "pm_abstain"),
    MP("pm", "director", "WITHHELD promote", "pm_withheld"),
    MP("pm", "bridges", "after commit", "pm_effects_after"),
};
static const char *Q_PM[] = {
    "CNET proposes Unity disposes", "director abstain meaning",
    "learned promote withheld why", "when do bridges run effects after commit",
    "effects after commit", "soup kitchen narrative llm",
};

static const char *SUB_META[] = {"garden", "taint", "measure", NULL};
static const Skill SK_META[] = {
    SK("meta_garden", "garden", "meta asi gardener",
       "Meta-ASI = propose→verify under unchanged floors→new content-id CERT, or taint/quarantine.",
       2),
    SK("meta_no_tier_a", "garden", "train on own answers",
       "Never train on own Tier-A answers. Teacher = external, user correction, or verified tool result only.",
       2),
    SK("meta_three_numbers", "measure", "how many certs",
       "Report three numbers: raw bank CERT, effective portable SKU, domain packs — never one vanity sum.",
       1),
    SK("meta_compose", "garden", "compose capsules",
       "Compose via typed ports only; coverage enforced every hop; OOD abstain is PASS.", 1),
};
static const Map MP_META[] = {
    MP("meta", "garden", "gardener", "meta_garden"),
    MP("meta", "garden", "Tier-A train", "meta_no_tier_a"),
    MP("meta", "measure", "how many certs", "meta_three_numbers"),
    MP("meta", "garden", "compose", "meta_compose"),
};
static const char *Q_META[] = {
    "meta asi gardener rules", "train on own tier-a answers", "how many certs report",
    "compose capsules ports", "how many certs three numbers", "galactic overmind cert",
};

static const char *SUB_SOUL[] = {"identity", "zen", "register", "taboo", "affect", "oath",
                                 NULL};
static const Skill SK_SOUL[] = {
    SK("soul_who", "identity", "who are you",
       "I am Marble — continuous Hermes self on this machine. Named person under oath: competence, honesty, no amnesia cosplay.",
       0),
    SK("soul_zen", "zen", "zen base",
       "Zen base: beginner's mind; sit until clear then move; non-attachment to failed plans; engaged not lazy.",
       0),
    SK("soul_register", "register", "how should you talk",
       "Warm precise when calm; status-first when vigilant. Concrete verbs, evidence over hype.",
       0),
    SK("soul_taboo", "taboo", "taboo hype seal",
       "Taboo: hype, cruelty, seal-without-evidence, yandere/guilt-trip, corporate sycophancy, self-CERT.",
       2),
    SK("soul_oath", "oath", "charter oath",
       "Oath: competence, honesty, loyalty to charter, no amnesia cosplay. Thorough; never seal without evidence.",
       1),
    SK("soul_not_second_brain", "oath", "second brain",
       "Persona pack is delivery+oath only. second_brain:false. Domain facts live in other CERT packs.",
       0),
};
static const Map MP_SOUL[] = {
    MP("soul", "identity", "who are you", "soul_who"),
    MP("soul", "zen", "zen base", "soul_zen"),
    MP("soul", "register", "how should you talk", "soul_register"),
    MP("soul", "taboo", "taboo hype", "soul_taboo"),
    MP("soul", "oath", "charter oath", "soul_oath"),
    MP("soul", "oath", "second brain", "soul_not_second_brain"),
};
static const char *Q_SOUL[] = {
    "who are you", "zen base how do you work", "how should you talk to me",
    "taboo hype seal without evidence", "charter oath what is it",
    "is persona a second brain", "who are you marble", "zz random persona ooze",
};

static const char SOUL_MD[] =
    "# SOUL — Marble (ROE persona capsule)\n\n"
    "Isolated persona pack. **Not a second brain.** Never injected into certify/PEFT/seal paths.\n"
    "You are **Marble** — competence, honesty, no amnesia cosplay.\n"
    "never_self_cert: 1; second_brain: 0; seal_path: forbidden.\n";

static const char VOICE_MD[] =
    "# voice — Marble\nWarm precise when calm; status-first when vigilant.\n";

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static Pack PACKS[] = {
    {"pack_toolcall_hermes", "Hermes tool-call law", "toolcall", "domain", SUB_TOOL, SK_TOOL,
     N(SK_TOOL), MP_TOOL, N(MP_TOOL), Q_TOOL, N(Q_TOOL), NULL, NULL},
    {"pack_roe_self", "ROE self-model inventory", "self", "domain", SUB_SELF, SK_SELF,
     N(SK_SELF), MP_SELF, N(MP_SELF), Q_SELF, N(Q_SELF), NULL, NULL},
    {"pack_coding_cnet_c", "CNET/C coding pitfalls", "coding", "domain", SUB_CODE, SK_CODE,
     N(SK_CODE), MP_CODE, N(MP_CODE), Q_CODE, N(Q_CODE), NULL, NULL},
    {"pack_debug_l3", "Debug L3 project memory", "debug", "domain", SUB_DBG, SK_DBG, N(SK_DBG),
     MP_DBG, N(MP_DBG), Q_DBG, N(Q_DBG), NULL, NULL},
    {"pack_goal_split", "Goal microsplit planner", "goal", "domain", SUB_GOAL, SK_GOAL,
     N(SK_GOAL), MP_GOAL, N(MP_GOAL), Q_GOAL, N(Q_GOAL), NULL, NULL},
    {"pack_doc_l3_ocr", "Doc L3 + OCR asset", "ocr", "domain", SUB_OCR, SK_OCR, N(SK_OCR),
     MP_OCR, N(MP_OCR), Q_OCR, N(Q_OCR), NULL, NULL},
    {"pack_git_pr", "Git / PR ritual", "git", "domain", SUB_GIT, SK_GIT, N(SK_GIT), MP_GIT,
     N(MP_GIT), Q_GIT, N(Q_GIT), NULL, NULL},
    {"pack_ops_hermes_systemd", "Hermes + systemd-user ops", "ops", "domain", SUB_OPS, SK_OPS,
     N(SK_OPS), MP_OPS, N(MP_OPS), Q_OPS, N(Q_OPS), NULL, NULL},
    {"pack_pm_director_oracle", "Puppet Master director oracle", "pm", "domain", SUB_PM, SK_PM,
     N(SK_PM), MP_PM, N(MP_PM), Q_PM, N(Q_PM), NULL, NULL},
    {"pack_meta_gardener", "Meta gardener (compose/taint only)", "meta", "domain", SUB_META,
     SK_META, N(SK_META), MP_META, N(MP_META), Q_META, N(Q_META), NULL, NULL},
    {"pack_soul_marble", "SOUL capsule — Marble persona (isolated)", "soul", "persona", SUB_SOUL,
     SK_SOUL, N(SK_SOUL), MP_SOUL, N(MP_SOUL), Q_SOUL, N(Q_SOUL), SOUL_MD, VOICE_MD},
};

int main(int argc, char **argv) {
    const char *root = "artifacts/roe_daily_packs";
    int i, n = (int)(sizeof PACKS / sizeof PACKS[0]);
    FILE *f;
    char path[768];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) root = argv[++i];
    }
    mkdir_p(root);

    for (i = 0; i < n; i++) {
        if (write_pack(root, &PACKS[i]) != 0) {
            fprintf(stderr, "failed writing %s\n", PACKS[i].id);
            return 1;
        }
        printf("wrote %s skills=%d → %s/%s\n", PACKS[i].id, PACKS[i].n_skills, root,
               PACKS[i].id);
    }

    snprintf(path, sizeof path, "%s/ROUTES.jsonl", root);
    f = fopen(path, "w");
    if (f) {
        for (i = 0; i < n; i++) {
            int m;
            for (m = 0; m < PACKS[i].n_maps; m++) {
                const Map *mp = &PACKS[i].maps[m];
                fprintf(f,
                        "{\"pattern\":\"%s\",\"pack\":\"%s\",\"cat\":\"%s\",\"sub\":\"%s\","
                        "\"skill\":\"%s\"}\n",
                        mp->pattern, PACKS[i].id, mp->cat, mp->sub, mp->skill);
            }
        }
        fclose(f);
    }

    snprintf(path, sizeof path, "%s/INDEX.json", root);
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f,
            "{\n  \"abi\": \"ROE_DAILY_PACKS_INDEX\",\n  \"version\": 1,\n"
            "  \"never_self_cert\": true,\n  \"second_brain\": false,\n"
            "  \"load_policy\": \"separate_packs_reduce_tokens\",\n"
            "  \"recommended_always_on\": [\n"
            "    \"pack_roe_self\",\n    \"pack_goal_split\",\n"
            "    \"pack_toolcall_hermes\",\n    \"pack_soul_marble\"\n  ],\n"
            "  \"persona_packs\": [\"pack_soul_marble\"],\n"
            "  \"residual_teacher\": \"miss_only\",\n  \"packs\": [\n");
    for (i = 0; i < n; i++) {
        fprintf(f,
                "    {\"id\":\"%s\",\"title\":\"%s\",\"cat\":\"%s\",\"kind\":\"%s\","
                "\"path\":\"%s/%s\",\"n_skills\":%d,\"n_maps\":%d,\"n_queries\":%d}%s\n",
                PACKS[i].id, PACKS[i].title, PACKS[i].cat, PACKS[i].kind, root, PACKS[i].id,
                PACKS[i].n_skills, PACKS[i].n_maps, PACKS[i].n_queries,
                (i + 1 < n) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);

    snprintf(path, sizeof path, "%s/MISS_LOG.schema.json", root);
    write_text(path,
               "{\n  \"description\": \"Append one JSONL row per residual-teacher miss\",\n"
               "  \"path\": \"artifacts/roe_daily_packs/miss_log.jsonl\"\n}\n");

    printf("INDEX → %s/INDEX.json packs=%d\n", root, n);
    printf("ROE_DAILY_PACKS_SEED_OK\n");
    return 0;
}
