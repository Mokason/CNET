#include "../include/agent_memory.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define MAX_TURNS 256
#define MAX_CONTENT 512

typedef struct {
    char role[16];
    char content[MAX_CONTENT];
    char timestamp[32];
} AgentTurn;

static AgentTurn chat_history[MAX_TURNS];
static AgentTurn thoughts[MAX_TURNS];
static size_t chat_count = 0;
static size_t thought_count = 0;
static int initialized = 0;

/* Early declaration for knowledge (full def later) */
#define MAX_KNOWLEDGE_CHUNKS 512
typedef struct {
    char source[64];
    char entities[256];
    char snippet[512];
} KnowledgeChunk;
static KnowledgeChunk knowledge_chunks[MAX_KNOWLEDGE_CHUNKS];
static size_t knowledge_count = 0;

/* Forward prototypes for KB (defined after KnowledgeChunk) */
static void load_knowledge_base(void);
static int save_knowledge_base(void);
static void load_knowledge_index(void);
static int save_knowledge_index(void);

/* Simple timestamp */
static void get_timestamp(char *buf, size_t cap) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!buf || cap == 0) return;
    if (tm && strftime(buf, cap, "%Y-%m-%d %H:%M:%S", tm) > 0) return;
    snprintf(buf, cap, "%s", "unknown-time");
}

static void trim_line(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) s[--len] = '\0';
}

int agent_memory_init(void) {
    if (initialized) return 0;

    chat_count = 0;
    thought_count = 0;
    knowledge_count = 0;

    /* Prefer single compact KB */
    load_knowledge_base();

    if (chat_count == 0 && thought_count == 0) {
        /* legacy migration path */
        {
            FILE *test = fopen(AGENT_CHAT_FILE, "r");
            if (!test) {
                FILE *old = fopen("agent_chat_history.txt", "r");
                if (old) { fclose(old); rename("agent_chat_history.txt", AGENT_CHAT_FILE); }
            } else { fclose(test); }

            test = fopen(AGENT_THOUGHTS_FILE, "r");
            if (!test) {
                FILE *old = fopen("agent_thoughts.txt", "r");
                if (old) { fclose(old); rename("agent_thoughts.txt", AGENT_THOUGHTS_FILE); }
            } else { fclose(test); }
        }

        /* load legacy chat */
        FILE *f = fopen(AGENT_CHAT_FILE, "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f) && chat_count < MAX_TURNS) {
                trim_line(line);
                char *role_start = strstr(line, "] ");
                if (!role_start) continue;
                role_start += 2;
                char *colon = strchr(role_start, ':');
                if (!colon) continue;
                *colon = '\0';
                char *content = colon + 2;

                strncpy(chat_history[chat_count].role, role_start, sizeof(chat_history[0].role)-1);
                strncpy(chat_history[chat_count].content, content, MAX_CONTENT-1);
                char *ts_end = strchr(line, ']');
                if (ts_end) {
                    size_t tslen = ts_end - (line + 1);
                    if (tslen > 0 && tslen < sizeof(chat_history[0].timestamp)) {
                        strncpy(chat_history[chat_count].timestamp, line+1, tslen);
                        chat_history[chat_count].timestamp[tslen] = '\0';
                    }
                }
                chat_count++;
            }
            fclose(f);
        }

        /* legacy thoughts */
        f = fopen(AGENT_THOUGHTS_FILE, "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f) && thought_count < MAX_TURNS) {
                trim_line(line);
                char *role_start = strstr(line, "] ");
                if (!role_start) continue;
                role_start += 2;
                char *colon = strchr(role_start, ':');
                if (!colon) continue;
                *colon = '\0';
                char *content = colon + 2;

                strncpy(thoughts[thought_count].role, role_start, sizeof(thoughts[0].role)-1);
                strncpy(thoughts[thought_count].content, content, MAX_CONTENT-1);
                char *ts_end = strchr(line, ']');
                if (ts_end) {
                    size_t tslen = ts_end - (line + 1);
                    if (tslen > 0 && tslen < sizeof(thoughts[0].timestamp)) {
                        strncpy(thoughts[thought_count].timestamp, line+1, tslen);
                        thoughts[thought_count].timestamp[tslen] = '\0';
                    }
                }
                thought_count++;
            }
            fclose(f);
        }

        /* legacy knowledge index */
        load_knowledge_index();

        /* one-time stack to KB */
        (void)save_knowledge_base();
    }

    initialized = 1;
    return 0;
}

int agent_record_turn(const char *role, const char *content) {
    if (!initialized) agent_memory_init();
    if (!role || !content || chat_count >= MAX_TURNS) return -1;

    char ts[32];
    get_timestamp(ts, sizeof(ts));

    strncpy(chat_history[chat_count].role, role, sizeof(chat_history[0].role)-1);
    chat_history[chat_count].role[sizeof(chat_history[0].role)-1] = '\0';
    strncpy(chat_history[chat_count].content, content, MAX_CONTENT-1);
    chat_history[chat_count].content[MAX_CONTENT-1] = '\0';
    snprintf(chat_history[chat_count].timestamp, sizeof(chat_history[0].timestamp), "%s", ts);

    chat_count++;

    /* Stack into single knowledge base file (smaller, one file). Publish is
       part of the operation: a failed durable write rolls back the count. */
    if (save_knowledge_base() != 0) {
        --chat_count;
        return -1;
    }
    return 0;
}

int agent_record_user(const char *content) {
    return agent_record_turn(AGENT_ROLE_USER, content);
}

int agent_record_assistant(const char *content) {
    return agent_record_turn(AGENT_ROLE_ASSISTANT, content);
}

int agent_record_thought(const char *content) {
    if (!initialized) agent_memory_init();
    if (!content || thought_count >= MAX_TURNS) return -1;

    char ts[32];
    get_timestamp(ts, sizeof(ts));

    strncpy(thoughts[thought_count].role, AGENT_ROLE_THOUGHT, sizeof(thoughts[0].role)-1);
    thoughts[thought_count].role[sizeof(thoughts[0].role)-1] = '\0';
    strncpy(thoughts[thought_count].content, content, MAX_CONTENT-1);
    thoughts[thought_count].content[MAX_CONTENT-1] = '\0';
    snprintf(thoughts[thought_count].timestamp, sizeof(thoughts[0].timestamp), "%s", ts);

    thought_count++;

    /* Stack into single knowledge base file. */
    if (save_knowledge_base() != 0) {
        --thought_count;
        return -1;
    }
    return 0;
}

int agent_get_chat_context(char *out, size_t cap, int max_turns, int include_thoughts) {
    if (!out || cap == 0) return -1;
    if (!initialized) agent_memory_init();

    out[0] = '\0';
    size_t start = (chat_count > (size_t)max_turns) ? chat_count - max_turns : 0;
    size_t written = 0;

    for (size_t i = start; i < chat_count; i++) {
        const char *prefix = "";
        if (strcmp(chat_history[i].role, AGENT_ROLE_USER) == 0) prefix = "User: ";
        else if (strcmp(chat_history[i].role, AGENT_ROLE_ASSISTANT) == 0) prefix = "Assistant: ";
        else if (strcmp(chat_history[i].role, AGENT_ROLE_THOUGHT) == 0 && include_thoughts) prefix = "[Thought]: ";
        else continue;

        int n = snprintf(out + written, cap - written, "%s%s\n", prefix, chat_history[i].content);
        if (n < 0 || (size_t)n >= cap - written) break;
        written += n;
    }
    return (int)written;
}

int agent_get_recent_thoughts(char *out, size_t cap, int max_thoughts) {
    if (!out || cap == 0) return -1;
    if (!initialized) agent_memory_init();

    out[0] = '\0';
    size_t start = (thought_count > (size_t)max_thoughts) ? thought_count - max_thoughts : 0;
    size_t written = 0;

    for (size_t i = start; i < thought_count; i++) {
        int n = snprintf(out + written, cap - written, "- %s\n", thoughts[i].content);
        if (n < 0 || (size_t)n >= cap - written) break;
        written += n;
    }
    return (int)written;
}

int agent_get_session_summary(char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    if (!initialized) agent_memory_init();

    snprintf(out, cap,
             "Session: %zu chat turns, %zu internal thoughts loaded.",
             chat_count, thought_count);
    return 0;
}

int agent_save_session(void) {
    if (!initialized && agent_memory_init() != 0) return -1;
    return save_knowledge_base();
}

void agent_clear_session(void) {
    chat_count = 0;
    thought_count = 0;
}

int agent_reload(void) {
    initialized = 0;
    return agent_memory_init();
}

/* === 7B: Simple scalable index for book knowledge ===
   We maintain a separate list of knowledge chunks (from book thoughts).
   Index is a persisted list of "entity|snippet" for fast lookup.
   Recall prioritizes exact entity matches then keyword overlap. */

static int index_built = 0;

/* === Consolidated single-file Knowledge Base (after structs) === */

#define KB_MAGIC 0x434E4B42u  /* 'CNKB' */
#define KB_VERSION 2u
#define KB_FNV_OFFSET UINT64_C(1469598103934665603)
#define KB_FNV_PRIME  UINT64_C(1099511628211)

static uint64_t kb_checksum_update(uint64_t hash, const void *data, size_t size) {
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    for (i = 0; i < size; ++i) {
        hash ^= (uint64_t)p[i];
        hash *= KB_FNV_PRIME;
    }
    return hash;
}

static int kb_path(char *out, size_t cap, const char *leaf) {
    const char *dir = getenv("CNET_AGENT_MEMORY_DIR");
    int n;
    if (out == NULL || cap == 0 || leaf == NULL) return -1;
    if (dir != NULL && dir[0] != '\0') {
        size_t len = strlen(dir);
        const char *sep = (dir[len - 1] == '/' || dir[len - 1] == '\\') ? "" : "/";
        n = snprintf(out, cap, "%s%s%s", dir, sep, leaf);
    } else {
        n = snprintf(out, cap, "%s", leaf);
    }
    return (n >= 0 && (size_t)n < cap) ? 0 : -1;
}

static int kb_sync_file(FILE *f) {
    if (fflush(f) != 0) return -1;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0 ? 0 : -1;
#else
    return fsync(fileno(f)) == 0 ? 0 : -1;
#endif
}

static int kb_sync_parent(const char *path) {
#ifdef _WIN32
    (void)path;
    return 0;
#else
    char dir[1024];
    char *slash;
    int fd;
    int flags = O_RDONLY;
    int rc;
    size_t len;
    if (path == NULL) return -1;
    len = strlen(path);
    if (len >= sizeof dir) return -1;
    memcpy(dir, path, len + 1);
    slash = strrchr(dir, '/');
    if (slash == NULL) {
        snprintf(dir, sizeof dir, ".");
    } else if (slash == dir) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    fd = open(dir, flags);
    if (fd < 0) return -1;
    rc = fsync(fd);
    if (close(fd) != 0 && rc == 0) rc = -1;
    return rc == 0 ? 0 : -1;
#endif
}

static int kb_replace(const char *tmp_path, const char *path) {
#ifdef _WIN32
    return MoveFileExA(tmp_path, path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? 0 : -1;
#else
    return rename(tmp_path, path) == 0 ? 0 : -1;
#endif
}

/* Snapshot the current complete generation without creating a publication
   gap. The hard link preserves the old inode while the head remains live. */
static int kb_preserve_previous(const char *path) {
    char prev_path[1032];
    char prev_tmp[1040];
    int n;

    n = snprintf(prev_path, sizeof prev_path, "%s.prev", path);
    if (n < 0 || (size_t)n >= sizeof prev_path) return -1;
    n = snprintf(prev_tmp, sizeof prev_tmp, "%s.tmp", prev_path);
    if (n < 0 || (size_t)n >= sizeof prev_tmp) return -1;
    (void)remove(prev_tmp);
#ifdef _WIN32
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    }
    if (!CopyFileA(path, prev_tmp, FALSE)) return -1;
#else
    if (link(path, prev_tmp) != 0) {
        if (errno == ENOENT) return 0;
        return -1;
    }
#endif
    if (kb_replace(prev_tmp, prev_path) != 0) {
        (void)remove(prev_tmp);
        return -1;
    }
    return kb_sync_parent(prev_path);
}

static int kb_write(FILE *f, const void *data, size_t size, size_t count,
                    long fail_after, long *writes) {
    if (count == 0) return 0;
    if (fail_after >= 0 && *writes >= fail_after) {
        errno = EIO;
        return -1;
    }
    if (fwrite(data, size, count, f) != count) return -1;
    ++*writes;
    return 0;
}

/* Return 1 for a verified generation, 0 when absent, and -1 when invalid. */
static int kb_load_file(const char *path) {
    FILE *f;
    uint32_t magic = 0, ver = 0, nchat = 0, nthought = 0, nknow = 0;
    uint64_t stored_payload = 0, stored_checksum = 0;
    uint64_t expected_payload;
    uint64_t checksum = KB_FNV_OFFSET;
    int trailing;

    f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? 0 : -1;

    if (fread(&magic, sizeof magic, 1, f) != 1 || magic != KB_MAGIC ||
        fread(&ver, sizeof ver, 1, f) != 1 ||
        fread(&nchat, sizeof nchat, 1, f) != 1 ||
        fread(&nthought, sizeof nthought, 1, f) != 1 ||
        fread(&nknow, sizeof nknow, 1, f) != 1 ||
        (ver != 1u && ver != KB_VERSION) ||
        nchat > MAX_TURNS || nthought > MAX_TURNS ||
        nknow > MAX_KNOWLEDGE_CHUNKS) {
        goto invalid;
    }

    expected_payload = (uint64_t)nchat * sizeof(AgentTurn) +
                       (uint64_t)nthought * sizeof(AgentTurn) +
                       (uint64_t)nknow * sizeof(KnowledgeChunk);
    if (ver == KB_VERSION &&
        (fread(&stored_payload, sizeof stored_payload, 1, f) != 1 ||
         fread(&stored_checksum, sizeof stored_checksum, 1, f) != 1 ||
         stored_payload != expected_payload)) {
        goto invalid;
    }

    if ((nchat > 0 && fread(chat_history, sizeof(AgentTurn), nchat, f) != nchat) ||
        (nthought > 0 && fread(thoughts, sizeof(AgentTurn), nthought, f) != nthought) ||
        (nknow > 0 && fread(knowledge_chunks, sizeof(KnowledgeChunk), nknow, f) != nknow)) {
        goto invalid;
    }
    trailing = fgetc(f);
    if (trailing != EOF) goto invalid;

    if (ver == KB_VERSION) {
        checksum = kb_checksum_update(checksum, chat_history,
                                      (size_t)nchat * sizeof(AgentTurn));
        checksum = kb_checksum_update(checksum, thoughts,
                                      (size_t)nthought * sizeof(AgentTurn));
        checksum = kb_checksum_update(checksum, knowledge_chunks,
                                      (size_t)nknow * sizeof(KnowledgeChunk));
        if (checksum != stored_checksum) goto invalid;
    }

    chat_count = nchat;
    thought_count = nthought;
    knowledge_count = nknow;
    fclose(f);
    return 1;

invalid:
    chat_count = 0;
    thought_count = 0;
    knowledge_count = 0;
    fclose(f);
    return -1;
}

static void load_knowledge_base(void) {
    char path[1024];
    char prev_path[1032];
    int current;
    int n;

    if (kb_path(path, sizeof path, AGENT_KB_FILE) != 0) return;
    current = kb_load_file(path);
    if (current == 1) return;

    n = snprintf(prev_path, sizeof prev_path, "%s.prev", path);
    if (n < 0 || (size_t)n >= sizeof prev_path) return;
    if (kb_load_file(prev_path) == 1 && current < 0) {
        /* A corrupt head must not replace the verified fallback on the next
           save. Remove only after the previous generation has been verified. */
        (void)remove(path);
        (void)kb_sync_parent(path);
    }
}

static int save_knowledge_base(void) {
    char path[1024];
    char tmp_path[1032];
    FILE *f = NULL;
    uint32_t magic = KB_MAGIC;
    uint32_t ver = KB_VERSION;
    uint32_t nchat = (uint32_t)chat_count;
    uint32_t nthought = (uint32_t)thought_count;
    uint32_t nknow = (uint32_t)knowledge_count;
    uint64_t payload = (uint64_t)nchat * sizeof(AgentTurn) +
                       (uint64_t)nthought * sizeof(AgentTurn) +
                       (uint64_t)nknow * sizeof(KnowledgeChunk);
    uint64_t checksum = KB_FNV_OFFSET;
    const char *inject = getenv("CNET_AGENT_MEMORY_FAIL_AFTER_WRITES");
    char *end = NULL;
    long fail_after = -1;
    long writes = 0;
    int n;

    if (inject != NULL && inject[0] != '\0') {
        long parsed = strtol(inject, &end, 10);
        if (end != inject && *end == '\0' && parsed >= 0) fail_after = parsed;
    }
    if (kb_path(path, sizeof path, AGENT_KB_FILE) != 0) return -1;
    n = snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp_path) return -1;

    checksum = kb_checksum_update(checksum, chat_history,
                                  (size_t)nchat * sizeof(AgentTurn));
    checksum = kb_checksum_update(checksum, thoughts,
                                  (size_t)nthought * sizeof(AgentTurn));
    checksum = kb_checksum_update(checksum, knowledge_chunks,
                                  (size_t)nknow * sizeof(KnowledgeChunk));

    (void)remove(tmp_path);
    f = fopen(tmp_path, "wb");
    if (f == NULL) return -1;
    {
        int ok = 1;
        if (kb_write(f, &magic, sizeof magic, 1, fail_after, &writes) != 0 ||
            kb_write(f, &ver, sizeof ver, 1, fail_after, &writes) != 0 ||
            kb_write(f, &nchat, sizeof nchat, 1, fail_after, &writes) != 0 ||
            kb_write(f, &nthought, sizeof nthought, 1, fail_after, &writes) != 0 ||
            kb_write(f, &nknow, sizeof nknow, 1, fail_after, &writes) != 0 ||
            kb_write(f, &payload, sizeof payload, 1, fail_after, &writes) != 0 ||
            kb_write(f, &checksum, sizeof checksum, 1, fail_after, &writes) != 0 ||
            kb_write(f, chat_history, sizeof(AgentTurn), nchat, fail_after, &writes) != 0 ||
            kb_write(f, thoughts, sizeof(AgentTurn), nthought, fail_after, &writes) != 0 ||
            kb_write(f, knowledge_chunks, sizeof(KnowledgeChunk), nknow,
                     fail_after, &writes) != 0 ||
            kb_sync_file(f) != 0) {
            ok = 0;
        }
        if (fclose(f) != 0) ok = 0;
        f = NULL;
        if (!ok) {
            (void)remove(tmp_path);
            return -1;
        }
    }
    if (kb_preserve_previous(path) != 0) {
        (void)remove(tmp_path);
        return -1;
    }
    if (kb_replace(tmp_path, path) != 0) {
        (void)remove(tmp_path);
        return -1;
    }
    return kb_sync_parent(path);
}

static void load_knowledge_index(void) {
    if (index_built) return;

    /* If KB was already loaded in init, chunks are populated */
    if (knowledge_count > 0) {
        index_built = 1;
        return;
    }

    knowledge_count = 0;

    /* Fallback to legacy md only if no KB */
    FILE *f = fopen(AGENT_KNOWLEDGE_INDEX_FILE, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f) && knowledge_count < MAX_KNOWLEDGE_CHUNKS) {
        trim_line(line);
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        *p1 = 0;
        char *p2 = strchr(p1+1, '|');
        if (!p2) continue;
        *p2 = 0;
        snprintf(knowledge_chunks[knowledge_count].source, sizeof(knowledge_chunks[0].source), "%s", line);
        snprintf(knowledge_chunks[knowledge_count].entities, sizeof(knowledge_chunks[0].entities), "%s", p1+1);
        snprintf(knowledge_chunks[knowledge_count].snippet, sizeof(knowledge_chunks[0].snippet), "%s", p2+1);
        knowledge_count++;
    }
    fclose(f);
    index_built = 1;
}

static int save_knowledge_index(void) {
    /* Primary storage is now the single consolidated binary KB.
     * No .md sidecar export is written (avoids stray data .md files; use bin or in-memory queries).
     */
    return save_knowledge_base();
}

int agent_build_knowledge_index(void) {
    load_knowledge_index();
    if (!initialized) agent_memory_init();
    knowledge_count = 0;

    for (size_t i = 0; i < thought_count && knowledge_count < MAX_KNOWLEDGE_CHUNKS; i++) {
        const char *c = thoughts[i].content;
        if (strstr(c, "[ENTITIES:") == NULL) continue; // only book-like with entities
        char src[64] = "book";
        char *src_start = strstr(c, "[");
        if (src_start) {
            char *src_end = strchr(src_start+1, ']');
            if (src_end) {
                size_t sl = (size_t)(src_end - src_start - 1);
                if (sl >= sizeof(src)) sl = sizeof(src) - 1;
                if (sl > 0) memcpy(src, src_start + 1, sl);
                src[sl]=0;
            }
        }
        char ents[256] = {0};
        char *e_start = strstr(c, "[ENTITIES:");
        if (e_start) {
            e_start += 10;
            char *e_end = strchr(e_start, ']');
            if (e_end) {
                size_t el = (size_t)(e_end - e_start);
                if (el >= sizeof(ents)) el = sizeof(ents) - 1;
                if (el > 0) memcpy(ents, e_start, el);
                ents[el]=0;
            }
        }
        // snippet is the rest
        const char *snip = c;
        char *content_start = strstr(c, "] ");
        if (content_start) snip = content_start + 2;

        snprintf(knowledge_chunks[knowledge_count].source, sizeof(knowledge_chunks[0].source), "%s", src);
        snprintf(knowledge_chunks[knowledge_count].entities, sizeof(knowledge_chunks[0].entities), "%s", ents);
        snprintf(knowledge_chunks[knowledge_count].snippet, sizeof(knowledge_chunks[0].snippet), "%s", snip);
        knowledge_chunks[knowledge_count].source[sizeof(knowledge_chunks[0].source)-1]=0;
        knowledge_chunks[knowledge_count].entities[sizeof(knowledge_chunks[0].entities)-1]=0;
        knowledge_chunks[knowledge_count].snippet[sizeof(knowledge_chunks[0].snippet)-1]=0;
        knowledge_count++;
    }
    if (save_knowledge_index() != 0) return -1;
    index_built = 1;
    return (int)knowledge_count;
}

int agent_recall_knowledge_indexed(const char *query, char *out, size_t cap, int max_snippets) {
    load_knowledge_index();
    agent_build_knowledge_index(); // ensure fresh

    if (!query || !out || cap==0) return 0;
    out[0] = '\0';

    // tokenize query
    char qtokens[16][64]; int nq=0;
    char qcopy[256]; strncpy(qcopy, query, 255); qcopy[255]=0;
    char *t = strtok(qcopy, " \t,.?!");
    while (t && nq<16) { strncpy(qtokens[nq++], t, 63); t=strtok(NULL," \t,.?!"); }

    // score
    typedef struct {int idx; int score;} Sc; Sc scs[128]; int ns=0;
    for (size_t i=0; i<knowledge_count && ns<128; i++) {
        int sc = 0;
        const char *c = knowledge_chunks[i].snippet;
        const char *e = knowledge_chunks[i].entities;
        for (int j=0; j<nq; j++) {
            if (strstr(e, qtokens[j])) sc += 5; // entity match strong
            if (strstr(c, qtokens[j])) sc += 2;
        }
        if (sc > 0) { scs[ns].idx=(int)i; scs[ns].score=sc; ns++; }
    }

    // sort desc
    for (int a=0; a<ns-1;a++) for(int b=a+1;b<ns;b++) if(scs[b].score > scs[a].score){ Sc tmp=scs[a]; scs[a]=scs[b]; scs[b]=tmp; }

    size_t w=0; int got=0;
    for (int s=0; s<ns && got<max_snippets; s++) {
        const char *sn = knowledge_chunks[ scs[s].idx ].snippet;
        int n = snprintf(out+w, cap-w, "- %s\n", sn);
        if (n>0 && (size_t)n < cap-w) w += n;
        got++;
    }
    return got;
}

void sanitize_for_filename(const char *in, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j < cap - 1; i++) {
        char c = in[i];
        if (c == ' ' || c == '?' || c == '!' || c == ':' || c == '/' || c == '\\' ||
            c == '*' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\t' || c == '\n') {
            out[j++] = '_';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    if (j == 0) strcpy(out, "default");
}

/* === 2. Hierarchical memory + auto-summarization === */
int agent_create_summary(char *summary_out, size_t cap, int num_recent) {
    if (!summary_out || cap == 0) return 0;
    if (!initialized) agent_memory_init();

    summary_out[0] = '\0';
    size_t start = (thought_count > (size_t)num_recent) ? thought_count - num_recent : 0;
    char buf[512] = "Summary of recent: ";
    for (size_t i = start; i < thought_count; i++) {
        strncat(buf, thoughts[i].content, 80);
        strncat(buf, " ... ", sizeof(buf) - strlen(buf) - 1);
    }
    /* Simple condense: take key parts */
    strncpy(summary_out, buf, cap - 1);
    summary_out[cap-1] = '\0';
    /* Record as high level thought */
    char high_level[256];
    snprintf(high_level, sizeof(high_level), "[SUMMARY] %s", summary_out);
    agent_record_thought(high_level);
    return 1;
}

int agent_get_hierarchical_context(char *out, size_t cap, int max_levels) {
    (void)max_levels; /* reserved for future recursive multi-level expansion */
    if (!out || cap == 0) return 0;
    agent_create_summary(out, cap, 5); /* auto summarize recent */
    /* Could recurse for levels, but simple for now */
    return (int)strlen(out);
}

/* Better chunker: prefer paragraphs, then sentences, with size limit. 
   Also extract potential entities (Capitalized sequences) and tag. */
static void ingest_chunk(const char *source, const char *chunk, const char *entities) {
    char thought[1024];
    if (entities && entities[0]) {
        snprintf(thought, sizeof(thought), "[%s] [ENTITIES: %s] %s", source, entities, chunk);
    } else {
        snprintf(thought, sizeof(thought), "[%s] %s", source, chunk);
    }
    agent_record_thought(thought);
}

/* Simple entity extractor: sequences of 1+ capitalized words */
static void extract_entities(const char *text, char *entities, size_t cap) {
    entities[0] = '\0';
    const char *p = text;
    char current[128];
    size_t cur_len = 0;
    int in_entity = 0;
    size_t ent_written = 0;

    while (*p && ent_written < cap - 1) {
        if (isupper((unsigned char)*p)) {
            if (!in_entity) {
                if (cur_len > 0) { current[cur_len++] = ' '; }
                in_entity = 1;
            }
            if (cur_len < sizeof(current)-1) current[cur_len++] = *p;
        } else if (in_entity && (islower((unsigned char)*p) || *p == '\'' || *p == '-')) {
            if (cur_len < sizeof(current)-1) current[cur_len++] = *p;
        } else {
            if (in_entity && cur_len > 2) {
                current[cur_len] = '\0';
                if (ent_written > 0) { if (ent_written < cap-2) { entities[ent_written++] = ','; entities[ent_written++] = ' '; } }
                size_t to_copy = strlen(current);
                if (ent_written + to_copy < cap) {
                    strcpy(entities + ent_written, current);
                    ent_written += to_copy;
                }
            }
            cur_len = 0;
            in_entity = 0;
        }
        p++;
    }
    if (in_entity && cur_len > 2) {
        current[cur_len] = '\0';
        if (ent_written > 0 && ent_written < cap-2) { entities[ent_written++] = ','; entities[ent_written++] = ' '; }
        if (ent_written + strlen(current) < cap) strcpy(entities + ent_written, current);
    }
    entities[ent_written] = '\0';
}

int agent_ingest_text(const char *source_name, const char *text) {
    if (!source_name || !text) return 0;
    if (!initialized) agent_memory_init();

    int chunks = 0;
    const char *p = text;
    char chunk[512];
    size_t chunk_len = 0;
    char entities[256];

    /* Split preferring \n\n paragraphs, fallback to . or size */
    while (*p) {
        chunk[chunk_len++] = *p;

        int end_chunk = 0;
        if ((p[0] == '\n' && p[1] == '\n') || chunk_len >= 480) {
            end_chunk = 1;
        } else if (*p == '.' && chunk_len > 30) {
            /* look ahead small */
            if (p[1] == ' ' || p[1] == '\n' || p[1] == '\0') end_chunk = 1;
        }

        if (end_chunk && chunk_len > 15) {
            chunk[chunk_len] = '\0';
            extract_entities(chunk, entities, sizeof(entities));
            ingest_chunk(source_name, chunk, entities[0] ? entities : NULL);
            chunks++;
            chunk_len = 0;
        }
        p++;
        if (chunks > 100) break; /* safety */
    }
    if (chunk_len > 10) {
        chunk[chunk_len] = '\0';
        extract_entities(chunk, entities, sizeof(entities));
        ingest_chunk(source_name, chunk, entities[0] ? entities : NULL);
        chunks++;
    }
    return chunks;
}

int agent_ingest_file(const char *source_name, const char *path) {
    if (!source_name || !path) return -1;
    if (!initialized) agent_memory_init();

    FILE *f = fopen(path, "r");
    if (!f) {
        /* try to be helpful on error */
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to open book file: %s", path);
        agent_record_thought(msg);
        return -1;
    }

    /* Read whole file (assume reasonable size for demo) */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 1024*1024) { /* 1MB cap for safety */
        fclose(f);
        agent_record_thought("Book file too large or empty for ingest.");
        return -1;
    }

    char *content = (char *)malloc(fsize + 1);
    if (!content) {
        fclose(f);
        return -1;
    }
    size_t read = fread(content, 1, fsize, f);
    content[read] = '\0';
    fclose(f);

    int n = agent_ingest_text(source_name, content);
    free(content);
    return n;
}

/* Improved recall: scores by # of query word matches + entity bonus. Prioritizes entity-tagged. */
int agent_recall_knowledge(const char *query, char *out, size_t cap, int max_snippets) {
    if (!query || !out || cap == 0) return 0;
    if (!initialized) agent_memory_init();

    out[0] = '\0';
    size_t written = 0;
    int found = 0;

    /* Simple tokenization of query */
    char qtokens[16][64];
    int nq = 0;
    char qcopy[256];
    strncpy(qcopy, query, sizeof(qcopy)-1);
    qcopy[sizeof(qcopy)-1] = 0;
    char *tok = strtok(qcopy, " \t,.?!");
    while (tok && nq < 16) {
        strncpy(qtokens[nq++], tok, 63);
        tok = strtok(NULL, " \t,.?!");
    }

    /* Score thoughts */
    typedef struct { int idx; int score; } Scored;
    Scored scores[128];
    int nscores = 0;

    for (size_t i = 0; i < thought_count && nscores < 128; i++) {
        int score = 0;
        const char *c = thoughts[i].content;
        int has_entity = (strstr(c, "[ENTITIES:") != NULL);
        for (int t=0; t<nq; t++) {
            if (strstr(c, qtokens[t])) score += 2;
        }
        if (has_entity) score += 3;  /* bonus for extracted entities */
        if (score > 0) {
            scores[nscores].idx = (int)i;
            scores[nscores].score = score;
            nscores++;
        }
    }

    /* Sort by score desc (simple bubble for small N) */
    for (int a=0; a<nscores-1; a++) {
        for (int b=a+1; b<nscores; b++) {
            if (scores[b].score > scores[a].score) {
                Scored tmp = scores[a]; scores[a]=scores[b]; scores[b]=tmp;
            }
        }
    }

    for (int s=0; s<nscores && found < max_snippets; s++) {
        const char *c = thoughts[ scores[s].idx ].content;
        int n = snprintf(out + written, cap - written, "- %s\n", c);
        if (n > 0 && (size_t)n < cap-written) written += n;
        found++;
    }
    return found;
}

/* === HF / remote text source helpers for training generative models from https://huggingface.co/datasets ===
 * Adapted to be shared. Supports curl for remote JSONL/txt.
 */

#ifdef _WIN32
#define AGENT_POPOPEN _popen
#define AGENT_POPCLOSE _pclose
#else
#define AGENT_POPOPEN popen
#define AGENT_POPCLOSE pclose
#endif

static bool agent_starts_with_http(const char *src) {
    if (!src) return false;
    return (strncmp(src, "http://", 7) == 0) || (strncmp(src, "https://", 8) == 0);
}

FILE *agent_open_text_source(const char *source) {
    if (!source || source[0] == '\0') return NULL;
    if (agent_starts_with_http(source)) {
        char cmd[8192];
        /* Use curl.exe on Windows (common), -L follow, --silent */
        snprintf(cmd, sizeof(cmd), "curl.exe -L --silent --show-error \"%s\"", source);
        return AGENT_POPOPEN(cmd, "r");
    }
    return fopen(source, "r");
}

void agent_close_text_source(FILE *f, const char *source) {
    if (!f) return;
    if (source && agent_starts_with_http(source)) {
        AGENT_POPCLOSE(f);
    } else {
        fclose(f);
    }
}

bool agent_extract_json_string(const char *json_line, const char *key, char *out, size_t cap) {
    if (!json_line || !key || !out || cap == 0) return false;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json_line, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return false;
    ++p;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return false;
    ++p;

    size_t out_len = 0;
    while (*p) {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            unsigned char esc = (unsigned char)*p++;
            if (!esc) break;
            if (esc == 'n') c = '\n';
            else if (esc == 'r') c = '\r';
            else if (esc == 't') c = '\t';
            else if (esc == '"') c = '"';
            else if (esc == '\\') c = '\\';
            else c = esc;
        } else if (c == '"') {
            break;
        }
        if (out_len + 1 < cap) out[out_len++] = (char)c;
    }
    out[out_len] = '\0';
    return out_len > 0;
}