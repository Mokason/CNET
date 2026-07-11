/* Runtime health optimizer acceptance gate.
 *
 * One maintenance pass must FIX what evidence says is broken and IMPROVE
 * what evidence says is ready — through existing certified paths only:
 *
 *   - a tampered certified entry is caught by the audit (demoted), and only
 *     comes back FROZEN via retrain + a passing re-certify;
 *   - a parked fault gets its verified target from the CONTRACT when the
 *     input is in the exemplar table, from a TEACHER when it is not;
 *   - evidence promotes a fresh entry FUZZY -> PROVISIONAL, never further;
 *   - a shadow that out-scores its incumbent AND certifies is hot-swapped;
 *   - a runtime adapter refuses matrix retraining, so a demoted adapter
 *     stays RESET — honestly reported for the acquisition loop to rebuild;
 *   - a healthy registry is a proven NO-OP, and a zero-initialized config
 *     is a total no-op even on a sick registry (opt-in, zero-init = legacy).
 */

#include <stdio.h>
#include <string.h>

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"
#include "../include/specialist_health.h"

#define SYM 4

static int failures;
static int checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port sym_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void onehot_row(double *row, int hot) {
    int i;
    for (i = 0; i < SYM; i++) row[i] = (i == hot) ? 1.0 : 0.0;
}

/* contract lookup table for the health pass */
static struct { const char *name; const Contract *c; } table[8];
static size_t table_n;

static void table_put(const char *name, const Contract *c) {
    table[table_n].name = name;
    table[table_n].c = c;
    table_n++;
}

static const Contract *lookup(const char *name, void *ctx) {
    size_t i;
    (void)ctx;
    for (i = 0; i < table_n; i++)
        if (strcmp(table[i].name, name) == 0) return table[i].c;
    return NULL;
}

static int identity_forward(void *opaque,
                            const double *input, size_t input_count,
                            double *output, size_t output_count) {
    size_t i;
    (void)opaque;
    if (input_count != SYM || output_count != SYM) return -1;
    for (i = 0; i < SYM; i++) output[i] = input[i];
    return 0;
}

static void train_identity(BinaryTransformNetwork *btn, unsigned int seed,
                           Port in, Port out,
                           const double *inputs, const double *targets) {
    btn_init(btn, SYM, SYM, 8, 64, 0.5, seed);
    btn_set_ports(btn, in, out);
    btn_train_dynamic(btn, inputs, targets, SYM, 20000, 200, 1e-4, 1e-6);
}

int main(void) {
    Port in_port = sym_port("hs_in");
    Port out_port = sym_port("hs_out");
    double table_in[SYM][SYM], table_out[SYM][SYM];
    double bad_raw[SYM] = {0.5, 0.5, 0.5, 0.5};

    BinaryTransformNetwork subject, teacher, fresh, cand, adapter;
    Contract subject_c, teacher_c, adapter_c;
    Specialist spec;
    PrimitiveRegistry reg;
    SpecialistHealthConfig cfg, none;
    SpecialistHealthReport rep;
    SpecialistTrust trust;
    int i;

    for (i = 0; i < SYM; i++) {
        onehot_row(table_in[i], i);
        onehot_row(table_out[i], i);  /* identity domain */
    }

    memset(&subject_c, 0, sizeof subject_c);
    memset(&teacher_c, 0, sizeof teacher_c);
    memset(&adapter_c, 0, sizeof adapter_c);
    memset(&adapter, 0, sizeof adapter);
    registry_init(&reg);
    reg.lifecycle_enabled = 1;

    printf("== specialist health: fix and improve through certified paths ==\n");

    /* -- fixture: subject (PARTIAL 3-exemplar contract), teacher (full) -- */
    train_identity(&subject, 42, in_port, out_port,
                   (const double *)table_in, (const double *)table_out);
    train_identity(&teacher, 43, in_port, out_port,
                   (const double *)table_in, (const double *)table_out);
    check(contract_init_borrowed(&subject_c, "hs_subject", &subject,
                                 (const double *)table_in,
                                 (const double *)table_out, 3) == 0 &&
          contract_init_borrowed(&teacher_c, "hs_teacher", &teacher,
                                 (const double *)table_in,
                                 (const double *)table_out, SYM) == 0,
          "fixture contracts author (subject partial, teacher full)");
    check(specialist_wrap_btn(&spec, &subject, "hs_subject") == 0 &&
          specialist_admit(&reg, &spec, &subject_c) == 0 &&
          specialist_wrap_btn(&spec, &teacher, "hs_teacher") == 0 &&
          specialist_admit(&reg, &spec, &teacher_c) == 0,
          "subject and teacher admitted through the one door");
    table_put("hs_subject", &subject_c);
    table_put("hs_teacher", &teacher_c);

    specialist_health_config_defaults(&cfg);
    cfg.contracts = lookup;

    /* -- A: healthy registry is a proven no-op --------------------------- */
    check(specialist_health_pass(&reg, &cfg, &rep) == 0 &&
          rep.entries == 2 && rep.demoted_by_audit == 0 &&
          rep.labeled_from_contract == 0 && rep.labeled_via_teacher == 0 &&
          rep.heal_attempted == 0 && rep.healed == 0 &&
          rep.promoted_provisional == 0 && rep.shadows_promoted == 0 &&
          rep.reset_remaining == 0 &&
          rep.trust[SPECIALIST_TRUST_CERTIFIED] == 2,
          "healthy registry: pass reports all-zero actions (no-op)");

    /* -- B: zero-init config is a total no-op on a SICK registry --------- */
    subject.hidden_output_weights[0] += 1.0;  /* tamper */
    memset(&none, 0, sizeof none);
    check(specialist_health_pass(&reg, &none, &rep) == 0 &&
          rep.demoted_by_audit == 0 && rep.heal_attempted == 0 &&
          specialist_axes(&reg, "hs_subject", &trust, NULL) == 0 &&
          trust == SPECIALIST_TRUST_CERTIFIED,
          "zero-init config: nothing acts even on tampered weights");

    /* -- C: audit detects; no verified target -> honestly stays RESET ---- */
    check(specialist_health_pass(&reg, &cfg, &rep) == 0 &&
          rep.demoted_by_audit == 1 && rep.heal_attempted == 1 &&
          rep.healed == 0 && rep.reset_remaining == 1 &&
          specialist_axes(&reg, "hs_subject", &trust, NULL) == 0 &&
          trust == SPECIALIST_TRUST_DEMOTED,
          "tamper: audit demotes; heal refuses without a verified target");

    /* -- D: fault input IS in the contract -> contract labels, heal fixes  */
    check(registry_record_fault(&reg, "hs_subject", table_in[0], bad_raw) == 0,
          "fault parks with the failing input (RESET)");
    check(specialist_health_pass(&reg, &cfg, &rep) == 0 &&
          rep.labeled_from_contract == 1 && rep.heal_attempted == 1 &&
          rep.healed == 1 && rep.reset_remaining == 0 &&
          specialist_axes(&reg, "hs_subject", &trust, NULL) == 0 &&
          trust == SPECIALIST_TRUST_CERTIFIED,
          "contract-labeled fault: retrain + re-certify restores CERTIFIED");

    /* -- E: fault input NOT in the contract -> the teacher labels it ----- */
    check(registry_record_fault(&reg, "hs_subject", table_in[3], bad_raw) == 0,
          "out-of-contract fault parks (input absent from exemplars)");
    check(specialist_health_pass(&reg, &cfg, &rep) == 0 &&
          rep.labeled_from_contract == 0 && rep.labeled_via_teacher == 1 &&
          rep.healed == 1 && rep.reset_remaining == 0,
          "teacher-labeled fault: heal restores FROZEN via passing certify");

    /* -- F: evidence promotes FUZZY -> PROVISIONAL, never further -------- */
    train_identity(&fresh, 44, in_port, out_port,
                   (const double *)table_in, (const double *)table_out);
    check(registry_add(&reg, &fresh, "hs_fresh") == 0, "fresh entry registers");
    fresh.output_successes = 32;
    check(specialist_health_pass(&reg, &cfg, &rep) == 0 &&
          rep.promoted_provisional == 1 &&
          specialist_axes(&reg, "hs_fresh", &trust, NULL) == 0 &&
          trust == SPECIALIST_TRUST_EVIDENCED,
          "evidence promotes to EVIDENCED (proof still required for more)");

    /* -- G: shadow out-scores incumbent AND certifies -> hot swap -------- */
    train_identity(&cand, 45, in_port, out_port,
                   (const double *)table_in, (const double *)table_out);
    check(registry_add(&reg, &cand, "hs_cand") == 0 &&
          registry_set_shadow(&reg, "hs_cand", "hs_teacher") == 0,
          "candidate registers as shadow of the incumbent");
    cand.output_successes = 40;
    teacher.output_successes = 5;
    teacher.output_failures = 5;
    check(specialist_health_pass(&reg, &cfg, &rep) == 0 &&
          rep.shadows_promoted == 1 &&
          specialist_axes(&reg, "hs_cand", &trust, NULL) == 0 &&
          trust == SPECIALIST_TRUST_CERTIFIED &&
          specialist_axes(&reg, "hs_teacher", &trust, NULL) == 0 &&
          trust == SPECIALIST_TRUST_DEMOTED,
          "shadow hot-swap: candidate CERTIFIED, incumbent demoted");

    /* -- H: adapters refuse retraining; demotion is reported, not hidden - */
    {
        Port a_in = sym_port("hs_a_in");
        Port a_out = sym_port("hs_a_out");
        double a_tab[SYM][SYM];
        Specialist a_spec;
        static int adapter_ctx;
        int j;
        for (j = 0; j < SYM; j++) onehot_row(a_tab[j], j);
        check(btn_init_adapter(&adapter, SYM, SYM, &a_in, 1, &a_out, 1,
                               identity_forward, NULL, &adapter_ctx,
                               0x48530001ULL, SYM) == 0 &&
              contract_init_borrowed(&adapter_c, "hs_adapter", &adapter,
                                     (const double *)a_tab,
                                     (const double *)a_tab, SYM) == 0,
              "runtime adapter wraps with its own contract");
        memset(&a_spec, 0, sizeof a_spec);
        a_spec.kind = SPECIALIST_KIND_ORACLE;
        a_spec.btn = &adapter;
        a_spec.name = "hs_adapter";
        check(specialist_admit(&reg, &a_spec, &adapter_c) == 0,
              "adapter admitted certified");
        table_put("hs_adapter", &adapter_c);
        check(registry_set_state(&reg, "hs_adapter", PRIM_RESET) == 0 &&
              specialist_health_pass(&reg, &cfg, &rep) == 0 &&
              rep.heal_attempted >= 1 && rep.healed == 0 &&
              specialist_axes(&reg, "hs_adapter", &trust, NULL) == 0 &&
              trust == SPECIALIST_TRUST_DEMOTED,
              "demoted adapter stays RESET: repair is acquisition's job");
    }

    /* -- I: final histogram is the whole registry, honestly counted ------ */
    check(specialist_health_pass(&reg, &cfg, &rep) == 0 &&
          rep.trust[SPECIALIST_TRUST_CERTIFIED] +
          rep.trust[SPECIALIST_TRUST_EVIDENCED] +
          rep.trust[SPECIALIST_TRUST_UNCERTIFIED] +
          rep.trust[SPECIALIST_TRUST_DEMOTED] == reg.count,
          "trust histogram accounts for every entry");

    registry_free(&reg);
    contract_free(&subject_c);
    contract_free(&teacher_c);
    contract_free(&adapter_c);
    btn_free(&subject);
    btn_free(&teacher);
    btn_free(&fresh);
    btn_free(&cand);
    btn_free(&adapter);

    printf("SPECIALIST_HEALTH_%s checks=%d\n",
           failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
