#ifndef NN_H
#define NN_H

#include <stddef.h>
#include <stdatomic.h>

#include "cnet_export.h"

/* Interface contract for a primitive's input/output wire format.
   family/field_width/field_count describe REPRESENTATION: a ONEHOT 16 is a
   16-way one-hot whether it encodes a hex digit, a nucleotide, or a chess
   piece. The optional tag names MEANING ("nibble_value", "word_token"): when
   both sides of a handoff carry a tag, the tags must match; an empty tag is
   a wildcard (see port_compatible). Tags never substitute for representation
   -- they only further constrain it. */
typedef enum {
    PORT_RAW = 0,      /* untyped / legacy v1; bit-order unknown */
    PORT_ONEHOT,       /* one or more one-hot fields */
    PORT_BINARY_MSB,   /* binary, most significant bit first */
    PORT_BINARY_LSB,   /* binary, least significant bit first */
    PORT_EVIDENCE,     /* 5B: finite support distribution over symbols (top-k + weights) */
    PORT_CONCEPT       /* 6A: higher-level discrete concept port (auto-abstracted) */
} PortFamily;

#define PORT_TAG_MAX 32  /* tag buffer size, including the terminating nul */

typedef struct {
    PortFamily family;
    size_t field_width;  /* size of one field */
    size_t field_count;  /* repeated fields; total = field_width * field_count */
    char tag[PORT_TAG_MAX];  /* semantic tag; "" = untagged */
} Port;

#define BTN_MAX_INPUT_PORTS 8
#define BTN_MAX_OUTPUT_PORTS 8

/* Runtime adapter ABI: lets a non-BTN implementation participate in the
   existing contract/registry/planner/executor path without teaching every
   planner about every backend. The callback writes exactly output_count
   doubles and returns 0 on success. adapter_digest is supplied by the backend
   from stable model/artifact identity; function or context pointers are never
   treated as behavior identity. */
typedef int (*BtnAdapterForwardFn)(void *context,
                                   const double *input, size_t input_count,
                                   double *output, size_t output_count);
typedef void (*BtnAdapterReleaseFn)(void *context);

typedef struct {
    size_t input_count;
    size_t hidden_count;
    size_t max_hidden_count;
    double learning_rate;
    double *input_hidden;
    double *hidden_bias;
    double *hidden_output;
    double *hidden_output_weights;
    double output_bias;
    double last_output;
} NeuralNetwork;

typedef struct {
    size_t input_count;
    size_t output_count;
    size_t hidden_count;
    size_t max_hidden_count;
    double learning_rate;
    double *input_hidden;
    double *hidden_bias;
    double *hidden_output;
    double *hidden_output_weights;
    double *output_bias;
    double *last_output;
    int ternary_inference;
    double ternary_threshold;
    Port input_ports[BTN_MAX_INPUT_PORTS];
    size_t input_port_count;
    /* The flat output vector is partitioned at running offsets, one segment
       per output port; consumers take one segment (planner projection). */
    Port output_ports[BTN_MAX_OUTPUT_PORTS];
    size_t output_port_count;
    /* Learned reliability: execution-time outcomes of validating the RAW
       output (every segment) against the output ports, recorded by the
       executors. Runtime-only (zeroed on init/load, not persisted).

       On Windows/MinGW we use C11 atomics so that multiple threads (via
       OpenMP or otherwise) inside one exe can safely increment counters on
       shared primitives without data races. On other platforms we fall back
       to plain counters (single-thread or OpenMP atomic pragmas can be used). */
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    _Atomic unsigned long output_successes;
    _Atomic unsigned long output_failures;
#else
    unsigned long output_successes;
    unsigned long output_failures;
#endif
    BtnAdapterForwardFn adapter_forward;
    BtnAdapterReleaseFn adapter_release;
    void *adapter_context;
    unsigned long long adapter_digest;
    size_t adapter_cost;
    /* Training-only momentum coefficient (heavy-ball); 0 = plain SGD, which is
       BYTE-IDENTICAL to the pre-momentum path. Not serialized (a hyperparameter
       of how the weights were fit, not part of them), zeroed on init/load. Set
       via btn_set_momentum before btn_train_dynamic to converge in fewer
       epochs. */
    double momentum;
} BinaryTransformNetwork;

int nn_init(
    NeuralNetwork *nn,
    size_t input_count,
    size_t initial_hidden_count,
    size_t max_hidden_count,
    double learning_rate,
    unsigned int seed
);

void nn_free(NeuralNetwork *nn);

double nn_forward(NeuralNetwork *nn, const double *inputs);

void nn_train(
    NeuralNetwork *nn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t epochs
);

double nn_average_loss(
    NeuralNetwork *nn,
    const double *inputs,
    const double *targets,
    size_t sample_count
);

double nn_train_dynamic(
    NeuralNetwork *nn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t max_epochs,
    size_t growth_window,
    double target_loss,
    double min_improvement
);

int nn_predict_hex_digit(NeuralNetwork *nn, const double bits[4], char *digit);

int nn_predict_hex_string(
    NeuralNetwork *nn,
    const char *binary,
    char *hex_output,
    size_t hex_output_size
);

int nn_predict_ascii_char(NeuralNetwork *nn, char high_hex, char low_hex, char *ch);

int nn_predict_ascii_string_from_hex(
    NeuralNetwork *nn,
    const char *hex,
    char *text_output,
    size_t text_output_size
);

int nn_predict_text_from_binary(
    NeuralNetwork *nibble_nn,
    NeuralNetwork *char_nn,
    const char *binary,
    char *text_output,
    size_t text_output_size
);

int nn_save(const NeuralNetwork *nn, const char *path);

int nn_load(NeuralNetwork *nn, const char *path);

CNET_API int btn_init(
    BinaryTransformNetwork *btn,
    size_t input_count,
    size_t output_count,
    size_t initial_hidden_count,
    size_t max_hidden_count,
    double learning_rate,
    unsigned int seed
);

/* Initialize a runtime-only specialist facade. Ownership of `context` transfers
   only on success; btn_free invokes `release` exactly once when non-NULL.
   Adapter BTNs can be certified and planned normally, but matrix training and
   CNU persistence refuse them. `behavior_digest` must be stable and nonzero. */
CNET_API int btn_init_adapter(
    BinaryTransformNetwork *btn,
    size_t input_count,
    size_t output_count,
    const Port *input_ports,
    size_t input_port_count,
    const Port *output_ports,
    size_t output_port_count,
    BtnAdapterForwardFn forward,
    BtnAdapterReleaseFn release,
    void *context,
    unsigned long long behavior_digest,
    size_t cost_hint
);

CNET_API int btn_is_adapter(const BinaryTransformNetwork *btn);

CNET_API void btn_free(BinaryTransformNetwork *btn);

CNET_API const double *btn_forward(BinaryTransformNetwork *btn, const double *inputs);

/* Inference-only ternary mode.
   When enabled, weights and biases are quantized per-pass to {-1, 0, +1}.
   Threshold is the absolute dead-zone: if |w| <= threshold, quantize to 0.
   Training paths ignore this setting and stay in full precision. */
int btn_set_ternary_inference(
    BinaryTransformNetwork *btn,
    int enabled,
    double threshold
);

int btn_train(
    BinaryTransformNetwork *btn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t epochs
);

/* ---- dynamic training: status is a STATUS, not a number ------------
 *
 * btn_train_dynamic returns a loss, and for a long time it also smuggled
 * failure into that same double as -2.0 (plateau) or -1.0 (bad argument).
 * Every threshold check in this tree is written `loss <= bar`, and a
 * negative satisfies every positive bar -- so the one value meaning THIS
 * NET IS NOT FIT TO CERTIFY read as the best result possible, and
 * src/legacy/main.c persisted such a net without a word.
 *
 * New code should call btn_train_dynamic_checked and branch on the
 * status. The double-returning form is kept for the ~110 existing call
 * sites and now returns a value that CANNOT satisfy any finite threshold
 * (see btn_train_loss_is_success).
 */
typedef enum {
    /* Trained. *loss_out is the whole-dataset mean squared error,
       computed through the PUBLIC btn_forward, so a caller can
       reproduce it exactly. */
    BTN_TRAIN_OK = 0,
    /* Growth was exhausted, escapes were tried, and no new best appeared
       for BTN_TRAIN_STUCK_WINDOWS consecutive windows. The net is the
       best one seen, but it did not reach the target and must not be
       persisted or certified as though it had. */
    BTN_TRAIN_PLATEAU_STATUS = 1,
    /* NULL/adapter btn, no samples, or an allocation failure. Nothing
       was trained and *loss_out is not a measurement. */
    BTN_TRAIN_INVALID = 2
} BtnTrainStatus;

/* The failure value the compatibility form returns: positive infinity.
   It is ordered (unlike NaN, every comparison with which is false, so
   `loss > bar` would ALSO be false and an `if (loss > bar) reject` would
   pass it), it can never satisfy `loss <= bar` for any finite bar, and it
   prints as `inf` rather than as a plausible measurement. */
#define BTN_TRAIN_LOSS_FAILED (1.0 / 0.0)

/* Train, and say what happened. loss_out may be NULL. */
int btn_train_dynamic_checked(
    BinaryTransformNetwork *btn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t max_epochs,
    size_t growth_window,
    double target_loss,
    double min_improvement,
    double *loss_out
);

/* THE predicate for a caller holding only the double: 1 when this value
   is a real loss that met `target`, 0 for every failure encoding and for
   anything non-finite. A caller that persists or certifies on success
   must use this rather than writing `loss <= target` itself. */
int btn_train_loss_is_success(double loss);

/* The same question, asked of the scalar-output NeuralNetwork trainer.

   nn_train_dynamic returns `previous_loss` when the epoch budget runs
   out -- a plausible finite number carrying no signal that the run did
   NOT reach the target it was given. That is F2b's defect in the sibling
   API, and src/legacy/main.c persisted the nibble layer on it.

   The status codes are the BTN_TRAIN_* values above: one training
   vocabulary, not two. *loss_out is ALWAYS nn_average_loss over every
   sample of the net actually handed back, so a caller can reproduce it
   exactly, and the status is decided by that recomputation rather than
   by whichever number the loop happened to be holding. loss_out may be
   NULL. */
int nn_train_dynamic_checked(
    NeuralNetwork *nn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t max_epochs,
    size_t growth_window,
    double target_loss,
    double min_improvement,
    double *loss_out
);

/* Compatibility form. Returns the whole-dataset loss on success and
   BTN_TRAIN_LOSS_FAILED on plateau or invalid argument -- never a
   negative, and never a finite number that could pass for a good run. */
double btn_train_dynamic(
    BinaryTransformNetwork *btn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t max_epochs,
    size_t growth_window,
    double target_loss,
    double min_improvement
);

/* Set the training momentum coefficient (default 0 = plain SGD). Valid range
   [0, 1); values outside are clamped. Takes effect on the next
   btn_train_dynamic. Returns 0, or -1 for a NULL/adapter btn. */
int btn_set_momentum(BinaryTransformNetwork *btn, double momentum);

int btn_predict_bits(
    BinaryTransformNetwork *btn,
    const double *inputs,
    char *bits_output,
    size_t bits_output_size
);

int btn_predict_hex_symbol_value(
    BinaryTransformNetwork *btn,
    char hex_symbol,
    char *bits_output,
    size_t bits_output_size
);

int btn_predict_word_token(
    BinaryTransformNetwork *btn,
    const char *word,
    const char **vocabulary,
    size_t vocabulary_count,
    size_t max_word_len,
    char *word_output,
    size_t word_output_size
);

int nn_predict_word_from_hex(
    NeuralNetwork *char_nn,
    BinaryTransformNetwork *word_nn,
    const char *hex,
    const char **vocabulary,
    size_t vocabulary_count,
    size_t max_word_len,
    char *word_output,
    size_t word_output_size
);

int btn_predict_ascii_string_from_hex_pairs(
    BinaryTransformNetwork *char_nn,
    const char *hex,
    char *text_output,
    size_t text_output_size
);

int btn_predict_word_from_hex(
    BinaryTransformNetwork *char_nn,
    BinaryTransformNetwork *word_nn,
    const char *hex,
    const char **vocabulary,
    size_t vocabulary_count,
    size_t max_word_len,
    char *word_output,
    size_t word_output_size
);

/* Support for replacing runtime .txt loading with compile-time const data.
   See "replace txt files" task. The generator (make freeze) produces
   FrozenBTNData instances from the committed *.txt files. Using the frozen
   path eliminates text parsing and I/O for committed primitives while
   preserving exact byte-identical behavior and the human-readable .txt
   as source of truth. */
typedef struct {
    const char *name;
    size_t input_count;
    size_t output_count;
    size_t hidden_count;
    const Port *input_ports;
    size_t input_port_count;
    const Port *output_ports;
    size_t output_port_count;
    const double *input_hidden;           /* [input_count * hidden_count] */
    const double *hidden_bias;            /* [hidden_count] */
    const double *hidden_output_weights;  /* [hidden_count * output_count] */
    const double *output_bias;            /* [output_count] */
} FrozenBTNData;

int btn_init_frozen(BinaryTransformNetwork *btn, const FrozenBTNData *data);

int btn_predict_raw_binary_word_token(
    BinaryTransformNetwork *btn,
    const char *binary,
    const char **vocabulary,
    size_t vocabulary_count,
    char *word_output,
    size_t word_output_size
);

/* Authoring (single input): set a primitive's interface contract. Fails if a
   port's total (field_width * field_count) does not equal the matching count.
   Convenience wrapper over btn_set_input_ports with one input port. */
int btn_set_ports(
    BinaryTransformNetwork *btn,
    Port input_port,
    Port output_port
);

/* Authoring (multi input): set an array of independently-sourced input ports.
   n must be in [1, BTN_MAX_INPUT_PORTS] and the sum of input port totals must
   equal input_count; the output port total must equal output_count.
   Convenience wrapper over btn_set_io_ports with one output port. */
int btn_set_input_ports(
    BinaryTransformNetwork *btn,
    const Port *input_ports,
    size_t n,
    Port output_port
);

/* Authoring (full generality): n_in independently-sourced input ports and
   n_out independently-consumable output ports. Counts must be in
   [1, BTN_MAX_INPUT_PORTS] / [1, BTN_MAX_OUTPUT_PORTS]; the sums of port
   totals must equal input_count / output_count respectively. */
int btn_set_io_ports(
    BinaryTransformNetwork *btn,
    const Port *input_ports,
    size_t n_in,
    const Port *output_ports,
    size_t n_out
);

/* Set or clear a port's semantic tag. Tags are atoms over [A-Za-z0-9_], at
   most PORT_TAG_MAX-1 chars; "" clears (untagged). Returns 0 on success, -1
   on an overlong or ill-formed tag (the port is left unchanged). */
int port_set_tag(Port *port, const char *tag);

/* Laplace-smoothed reliability from recorded execution outcomes:
   (successes + 1) / (successes + failures + 2). A fresh primitive scores the
   0.5 prior; evidence moves it toward its observed output-validity rate.
   Planners rank same-role alternatives by this score. */
double btn_reliability(const BinaryTransformNetwork *btn);

/* Persist / restore reliability evidence separately from the weights: the
   weight file stays a pure function definition, the sidecar is deployment
   experience ("CNET_STATS 1\n<successes> <failures>"). Load REPLACES the
   counters; a missing or malformed sidecar returns -1 and leaves them
   untouched (treat as "no recorded history"). Evidence describes the weights
   it was gathered against: retraining a primitive invalidates its sidecar
   (nn_demo removes the sidecars of primitives it regenerates). */
int btn_save_stats(const BinaryTransformNetwork *btn, const char *path);
int btn_load_stats(BinaryTransformNetwork *btn, const char *path);

/* Static composition check: is a producer's output port compatible with a
   consumer's input port? If BOTH ports carry a semantic tag, the tags must
   match (an untagged port is a wildcard); then family, field_width and
   field_count must match (RAW falls back to total-width equality). Returns 1
   if compatible, else 0. */
int port_compatible(Port producer_output, Port consumer_input);

/* Runtime domain check: is an actual vector a valid member of a port's domain?
   ONEHOT  -> each field has exactly one value > 0.5
   BINARY_* -> every value is unambiguous (< 0.25 or > 0.75)
   RAW     -> always valid. Returns 1 if valid, else 0. */
int port_validate(Port port, const double *values);

/* Discretize a vector to a port's canonical form, writing field_width*field_count
   values into clean. ONEHOT -> argmax per field (winner 1.0, rest 0.0);
   BINARY_* -> threshold each value at 0.5; RAW -> copy unchanged.
   Returns 0 on success, -1 on bad arguments. */
int port_canonicalize(Port port, const double *raw, double *clean);

/* Robustness headroom: the minimum distance of `values` to the nearest
   canonicalization boundary -- how far a raw output sits from flipping.
   BINARY_* -> min over bits of |v - 0.5| (range [0, 0.5]; port_validate's
   unambiguity band is <= 0.25). ONEHOT -> min over fields of (top1 - top2)
   (range [0, 1]; 0 = a tie at the argmax). RAW -> 1.0 (no boundary, no
   constraint). Writes the margin to *out_margin. Returns 0, or -1 on bad
   arguments. port_validate asks "is it unambiguous"; this asks "by how much". */
int port_margin(Port port, const double *values, double *out_margin);

int btn_save(const BinaryTransformNetwork *btn, const char *path);

int btn_load(BinaryTransformNetwork *btn, const char *path);

#endif
