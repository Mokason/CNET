/* Showrunner: session mood, cooldowns, episodic ring for Marble Live.
 * Delivery/schedule only — never CERT, never seal.
 * Gate: make showrunner → SHOWRUNNER_PASS
 */
#ifndef CNET_SHOWRUNNER_H
#define CNET_SHOWRUNNER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_SR_TEXT 512
#define CNET_SR_ID 64
#define CNET_SR_MAX_TURNS 32
#define CNET_SR_MAX_NOTES 64
#define CNET_SR_NOTE 240

typedef struct {
    char peer[CNET_SR_ID];
    char query[CNET_SR_TEXT];
    char answer[CNET_SR_TEXT];
    char skill[CNET_SR_ID];
    char source[24];
    int miss;
    int may_voice;
    unsigned long ts_unix;
} CnetSrTurn;

typedef struct {
    char text[CNET_SR_NOTE];
    unsigned long ts_unix;
    char peer[CNET_SR_ID];
} CnetSrNote;

typedef struct {
    /* neuromod-ish affect in [0,1], clamped */
    float da;   /* reward / spark */
    float ht;   /* calm / remember */
    float ado;  /* fatigue / consolidate */
    unsigned cooldown_until_unix;
    unsigned turn_i;
    CnetSrTurn turns[CNET_SR_MAX_TURNS];
    unsigned n_turns; /* filled count up to MAX */
    CnetSrNote notes[CNET_SR_MAX_NOTES];
    unsigned n_notes;
    char last_action[CNET_SR_ID];
    char path_episodic[256]; /* optional durable jsonl */
    char feel[24];           /* last classified feeling word */
    char feel_why[CNET_SR_TEXT];
    char path_affect[256];   /* durable session hit counts */
    unsigned sess_hits;
    unsigned sess_bricks;
} CnetShowrunner;

void cnet_sr_init(CnetShowrunner *S);
void cnet_sr_set_episodic_path(CnetShowrunner *S, const char *path);
void cnet_sr_set_affect_path(CnetShowrunner *S, const char *path);
/* Governor snapshot + this-session CERT hits. Call after ingest. */
void cnet_sr_session_overlay(CnetShowrunner *S);

/* Clamp + homeostasis pull toward baseline. */
void cnet_sr_tick_homeostasis(CnetShowrunner *S, float baseline_da, float baseline_ht,
                              float baseline_ado, float homeo);

/* After a turn: record + nudge affect. */
void cnet_sr_on_turn(CnetShowrunner *S, const char *peer, const char *q,
                     const char *answer, const char *skill, const char *source,
                     int miss, int may_voice);

/* 1 if allowed to speak/act now (cooldown). */
int cnet_sr_cooldown_ok(const CnetShowrunner *S, unsigned now_unix);
void cnet_sr_set_cooldown(CnetShowrunner *S, unsigned now_unix, unsigned sec);

/* Episodic note (explicit remember). */
int cnet_sr_remember(CnetShowrunner *S, const char *peer, const char *note,
                     unsigned long ts_unix);

/* Recall up to max notes matching substr (case-insensitive); returns count. */
int cnet_sr_recall(const CnetShowrunner *S, const char *substr, char out[][CNET_SR_NOTE],
                   int max_out);

/* Simple action detect from query (allowlist). */
typedef enum {
    CNET_SR_ACT_NONE = 0,
    CNET_SR_ACT_REMEMBER,
    CNET_SR_ACT_RECALL,
    CNET_SR_ACT_PROPOSE_CAPSULE,
    CNET_SR_ACT_STATUS,
    CNET_SR_ACT_IDENTITY,
    CNET_SR_ACT_LEARN,
    CNET_SR_ACT_KNOW,
    CNET_SR_ACT_GAPS
} CnetSrAction;

CnetSrAction cnet_sr_detect_action(const char *query);

/* Persist last notes to path_episodic (append jsonl lines). */
int cnet_sr_flush_episodic(const CnetShowrunner *S);

/* Color a CERT utterance from mood. Never mutates the CERT fact string
 * in-place; writes prefix+cert into out. Returns 0. Delivery only. */
int cnet_sr_color_utterance(const CnetShowrunner *S, const char *cert,
                            char *out, size_t cap);

/* Whole-utterance mood ask. Live DA/HT/ADO → feeling word, not meters. */
int cnet_sr_mood_query(const char *query);
int cnet_sr_mood_line(CnetShowrunner *S, char *out, size_t cap);
int cnet_sr_mood_follow_query(const char *query);
int cnet_sr_mood_follow_line(CnetShowrunner *S, char *out, size_t cap);
int cnet_sr_mood_why_query(const char *query);
int cnet_sr_mood_why_line(CnetShowrunner *S, char *out, size_t cap);
/* Overlay live neuromod json (dopamine/serotonin/adenosine). 0 if applied. */
int cnet_sr_ingest_neuromod(CnetShowrunner *S, const char *json, size_t n);

/* Want/will from live feel + optional open gap. Not an I-like-X FAQ. */
int cnet_sr_want_query(const char *query);
int cnet_sr_want_skip_gap(const char *gap_q);
int cnet_sr_want_line(CnetShowrunner *S, const char *gap_q, char *out, size_t cap);
int cnet_sr_cando_query(const char *query);
int cnet_sr_choose_query(const char *query);
int cnet_sr_choose_line(CnetShowrunner *S, const char *query, char *out, size_t cap);

/* Greet/ack presence. Affect-shaped delivery, no meters, not soul_who. */
int cnet_sr_greet_query(const char *query);
int cnet_sr_greet_line(const CnetShowrunner *S, char *out, size_t cap);
int cnet_sr_ack_query(const char *query);
int cnet_sr_ack_line(const CnetShowrunner *S, char *out, size_t cap);

/* Live neuromod → SPARE / STEADY / WARM. Delivery only. */
#define CNET_SR_REG_STEADY 0
#define CNET_SR_REG_SPARE 1
#define CNET_SR_REG_WARM 2
int cnet_sr_register(const CnetShowrunner *S);

/* remind in 20m TEXT — file-backed, never CERT. */
int cnet_sr_remind_parse(const char *query, unsigned *sec_out, char *text,
                         size_t cap);
int cnet_sr_remind_add(const char *path, unsigned long now, unsigned sec,
                       const char *text);
int cnet_sr_remind_due(const char *path, unsigned long now, char *out,
                       size_t cap);
int cnet_sr_remind_list(const char *path, unsigned long now, char *out,
                        size_t cap);

#ifdef __cplusplus
}
#endif

#endif
