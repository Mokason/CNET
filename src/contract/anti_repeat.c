#include "../../include/contract/anti_repeat.h"
#include "../../include/nn.h"
#include "../../include/contract/contract.h"

#include <string.h>
#include <math.h>

/* Core anti-repeat logic, extracted so it can be used as contract / circuit guard.
 * This is deliberately not LM-specific.
 */

int contract_init_anti_repeat(Contract *c,
                              const Port *prev_port,
                              const Port *candidate_port,
                              const Port *out_port)
{
    if (!c || !prev_port || !candidate_port || !out_port) return -1;

    memset(c, 0, sizeof(*c));
    strncpy(c->name, ANTI_REPEAT_CONTRACT_NAME, CONTRACT_NAME_MAX-1);

    c->input_port_count = 2;
    c->input_ports[0] = *prev_port;
    c->input_ports[1] = *candidate_port;

    c->output_port_count = 1;
    c->output_ports[0] = *out_port;

    c->exemplar_count = 0;
    c->owns_data = 0;

    return 0;
}

int apply_anti_repeat_guard(const Port *prev_port,
                            const double *prev_vec,
                            const Port *cand_port,
                            const double *cand_vec,
                            int vocab_size,
                            double penalty,
                            int use_second,
                            int *out_idx)
{
    if (!prev_port || !cand_port || !cand_vec || !out_idx || vocab_size <= 0) return -1;

    /* Find previous token index from prev_vec (supports ONEHOT or argmax on EVIDENCE) */
    int prev_idx = -1;
    if (prev_vec) {
        double maxp = -1.0;
        for (int i = 0; i < vocab_size; i++) {
            if (prev_vec[i] > maxp) {
                maxp = prev_vec[i];
                prev_idx = i;
            }
        }
    }

    /* Score candidates, applying repeat penalty */
    int best = 0;
    double mx = cand_vec[0];
    int second = 0;
    double mx2 = -1.0;

    for (int v = 0; v < vocab_size; v++) {
        double sc = cand_vec[v];
        if (v == prev_idx) {
            sc *= (1.0 - penalty);   /* contract-style suppression */
        }
        if (sc > mx) {
            mx2 = mx;
            second = best;
            mx = sc;
            best = v;
        } else if (sc > mx2) {
            mx2 = sc;
            second = v;
        }
    }

    if (use_second && best == prev_idx && mx2 > 0.0) {
        best = second;
    }

    /* Safety: never return the exact previous if we can avoid it */
    if (best == prev_idx && second != prev_idx && mx2 > 0.0) {
        best = second;
    }

    *out_idx = best;
    return 0;
}

/* Simple trainer for an anti-repeat BTN (future work).
 * Can be used to learn a guard primitive that is then certified
 * against the anti_repeat contract and composed in circuits.
 */
double train_anti_repeat_btn(BinaryTransformNetwork *btn,
                             const double *prev_inputs,
                             const double *cand_inputs,
                             const double *targets,
                             size_t samples,
                             size_t max_epochs)
{
    if (!btn || !prev_inputs || !cand_inputs || !targets || samples == 0) return -1.0;

    /* caller must have initialized btn with correct dims:
     * input = prev + cand, output = next (same size as cand)
     */
    return btn_train_dynamic(btn, targets, targets, samples,
                             max_epochs, 200, 0.001, 0.01);
}



