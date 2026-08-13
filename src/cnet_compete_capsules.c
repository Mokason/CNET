#include "cnet_compete_capsules.h"

#include "contract/contract.h"
#include "contract/coverage.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned input_bits;
    unsigned output_bits;
    const char *input_tag;
    const char *output_tag;
} UnitSpec;

static const UnitSpec specs[CNET_COMPETE_UNIT_COUNT] = {
    {8, 8,  "byte_raw",       "byte_incr"},
    {8, 8,  "byte_incr",      "byte_dbl"},
    {8, 8,  "byte_dbl",       "byte_final"},
    {8, 14, "minutes_u8",     "seconds_u14"},
    {8, 8,  "byte_crc_in",    "crc8_atm_out"},
    {4, 1,  "policy_flags_v1", "access_decision_v1"}
};

const char *cnet_compete_unit_name(CnetCompeteUnit unit) {
    static const char *names[CNET_COMPETE_UNIT_COUNT] = {
        "increment_mod256", "double_mod256", "add3_mod256",
        "minutes_to_seconds", "crc8_atm", "access_policy_v1"
    };
    return unit >= 0 && unit < CNET_COMPETE_UNIT_COUNT ? names[unit] : NULL;
}

static unsigned crc8_atm(unsigned byte) {
    unsigned crc = byte & 255u;
    int bit;
    for (bit = 0; bit < 8; ++bit)
        crc = (crc & 0x80u) ? ((crc << 1) ^ 0x07u) & 255u
                            : (crc << 1) & 255u;
    return crc;
}

static unsigned reference_value(CnetCompeteUnit unit, unsigned input) {
    switch (unit) {
        case CNET_COMPETE_UNIT_INCREMENT: return (input + 1u) & 255u;
        case CNET_COMPETE_UNIT_DOUBLE: return (input * 2u) & 255u;
        case CNET_COMPETE_UNIT_ADD3: return (input + 3u) & 255u;
        case CNET_COMPETE_UNIT_MINUTES: return input * 60u;
        case CNET_COMPETE_UNIT_CRC8: return crc8_atm(input);
        case CNET_COMPETE_UNIT_POLICY: {
            unsigned admin = input & 1u, owner = (input >> 1) & 1u;
            unsigned mfa = (input >> 2) & 1u, suspended = (input >> 3) & 1u;
            return (admin || (owner && mfa)) && !suspended ? 1u : 0u;
        }
        default: return 0;
    }
}

static void encode_msb(double *bits, unsigned width, unsigned value) {
    unsigned i;
    for (i = 0; i < width; ++i)
        bits[i] = (double)((value >> (width - i - 1u)) & 1u);
}

static unsigned decode_msb(const double *bits, unsigned width) {
    unsigned i, value = 0;
    for (i = 0; i < width; ++i)
        value = (value << 1) | (bits[i] >= 0.5 ? 1u : 0u);
    return value;
}

static int synthesize_lookup(BinaryTransformNetwork *btn, CnetCompeteUnit unit,
                             double **inputs_out, double **outputs_out) {
    const UnitSpec *spec = &specs[unit];
    const size_t domain = (size_t)1u << spec->input_bits;
    const double gain = 24.0;
    double *inputs = NULL, *outputs = NULL;
    Port input_port = {PORT_BINARY_MSB, spec->input_bits, 1, ""};
    Port output_port = {PORT_BINARY_MSB, spec->output_bits, 1, ""};
    size_t state, bit;

    inputs = (double *)calloc(domain * spec->input_bits, sizeof *inputs);
    outputs = (double *)calloc(domain * spec->output_bits, sizeof *outputs);
    if (inputs == NULL || outputs == NULL ||
        btn_init(btn, spec->input_bits, spec->output_bits, domain, domain,
                 0.01, 1) != 0 ||
        port_set_tag(&input_port, spec->input_tag) != 0 ||
        port_set_tag(&output_port, spec->output_tag) != 0 ||
        btn_set_ports(btn, input_port, output_port) != 0) {
        free(inputs); free(outputs); btn_free(btn);
        return -1;
    }
    btn->ternary_inference = 0;
    for (state = 0; state < domain; ++state) {
        size_t ones = 0;
        unsigned target = reference_value(unit, (unsigned)state);
        encode_msb(inputs + state * spec->input_bits,
                   spec->input_bits, (unsigned)state);
        encode_msb(outputs + state * spec->output_bits,
                   spec->output_bits, target);
        for (bit = 0; bit < spec->input_bits; ++bit) {
            double expected = inputs[state * spec->input_bits + bit];
            btn->input_hidden[state * btn->input_count + bit] =
                expected == 1.0 ? gain : -gain;
            if (expected == 1.0) ++ones;
        }
        btn->hidden_bias[state] = gain * (0.5 - (double)ones);
        for (bit = 0; bit < spec->output_bits; ++bit) {
            double expected = outputs[state * spec->output_bits + bit];
            btn->hidden_output_weights[bit * btn->max_hidden_count + state] =
                expected == 1.0 ? gain : -gain;
        }
    }
    for (bit = 0; bit < spec->output_bits; ++bit) btn->output_bias[bit] = 0.0;
    *inputs_out = inputs;
    *outputs_out = outputs;
    return 0;
}

int cnet_compete_capsule_build(CnetBase *base, HybridAi *coverage,
                               CnetCompeteUnit unit,
                               CnetCompeteCapsuleBuildReport *report) {
    const UnitSpec *spec;
    const char *name = cnet_compete_unit_name(unit);
    BinaryTransformNetwork btn;
    Contract contract;
    ExhaustiveReport exhaustive;
    CertifyReport robust;
    double *inputs = NULL, *outputs = NULL;
    size_t domain;
    int rc = -1;

    if (report != NULL) memset(report, 0, sizeof *report);
    if (base == NULL || coverage == NULL || name == NULL) return -1;
    spec = &specs[unit];
    domain = (size_t)1u << spec->input_bits;
    memset(&btn, 0, sizeof btn);
    memset(&contract, 0, sizeof contract);
    memset(&exhaustive, 0, sizeof exhaustive);
    memset(&robust, 0, sizeof robust);
    if (synthesize_lookup(&btn, unit, &inputs, &outputs) != 0) goto done;
    if (contract_init_borrowed(&contract, name, &btn, inputs, outputs,
                               domain) != 0) goto done;
    if (btn_certify_exhaustive(&btn, &contract, domain, &exhaustive) != 0 ||
        exhaustive.domain_swept != domain || exhaustive.domain_illformed != 0 ||
        btn_certify_robust(&btn, &contract,
                           CNET_COMPETE_CAPSULE_MARGIN_FLOOR, &robust) != 0)
        goto done;
    if (cnb_add_unit(base, &btn, &contract, NULL) != 0) goto done;
    if (hybrid_coverage_record(coverage, btn.input_ports[0], btn.output_ports[0],
                               name, inputs, outputs, domain,
                               spec->input_bits, spec->output_bits) != 0)
        goto done;
    if (report != NULL) {
        report->domain_rows = domain;
        report->input_bits = spec->input_bits;
        report->output_bits = spec->output_bits;
        report->certified_rows = robust.passed;
        report->min_margin = robust.min_margin;
        report->behavior_digest = contract_btn_digest(&btn);
    }
    rc = 0;
done:
    contract_free(&contract);
    btn_free(&btn);
    free(outputs); free(inputs);
    return rc;
}

int cnet_compete_capsule_eval(const CnetBase *base, const HybridAi *coverage,
                              CnetCompeteUnit unit, unsigned input,
                              unsigned *output) {
    const UnitSpec *spec;
    const char *name = cnet_compete_unit_name(unit);
    BinaryTransformNetwork btn;
    Contract contract;
    CertifyReport certify;
    double encoded[8], canonical[14];
    const double *raw;
    unsigned limit;
    int rc = -1;

    if (base == NULL || coverage == NULL || output == NULL || name == NULL)
        return -1;
    spec = &specs[unit];
    limit = 1u << spec->input_bits;
    if (input >= limit) return 1;
    memset(&btn, 0, sizeof btn);
    memset(&contract, 0, sizeof contract);
    memset(&certify, 0, sizeof certify);
    if (cnb_get_unit(base, name, &btn, &contract) != 0) goto done;
    encode_msb(encoded, spec->input_bits, input);
    if (!hybrid_coverage_admits_exact(coverage, name, btn.input_ports[0],
                                      btn.output_ports[0], encoded,
                                      spec->input_bits)) {
        rc = 1;
        goto done;
    }
    if (btn_certify_robust(&btn, &contract,
                           CNET_COMPETE_CAPSULE_MARGIN_FLOOR, &certify) != 0)
        goto done;
    raw = btn_forward(&btn, encoded);
    if (raw == NULL || !port_validate(btn.output_ports[0], raw) ||
        port_canonicalize(btn.output_ports[0], raw, canonical) != 0)
        goto done;
    *output = decode_msb(canonical, spec->output_bits);
    rc = 0;
done:
    contract_free(&contract);
    btn_free(&btn);
    return rc;
}
