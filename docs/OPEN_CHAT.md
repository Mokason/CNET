# Residual chat boundary

The current daemon's legacy ROE serving branch hard-disables teacher-on-miss.
`CNET_TEACHER_ON_MISS=1` does not re-enable that branch; the previous restart
recipe was obsolete. `CNET_SELF_ANSWER` controls template presentation, not
certification or creation of a language-model backend.

A certified hit can answer from admitted local knowledge. A miss or probe may
receive a C-native template, but a presentation label is not evidence of a new
certified capability. Explicit capsule misses stay unverified and do not
fall through into a claimed verified answer.

Other residual/model interfaces and background teaching lanes remain distinct.
Configure and test those interfaces explicitly; do not infer that every daemon
request uses a reachable model endpoint.

See [daemon protocol](CNETD.md), [utterances](UTTERANCE.md),
[evidence acquisition](TEACH_PATH.md) and [managed inference](cnet_dotnet_inference_harness.md).
