# Native operational trace

The legacy filename names an explicit execution trace, not private model
deliberation. The [C API](../include/cnet_roe_cot.h) records parse, split,
retrieve, check, act, join, verify, show and stop events.

```sh
make roe_chain_think
bin/roe_chain_think --no-act 'plan only'
```

`--no-act` suppresses front-door execution. Normal execution may invoke the
front door; `--teacher` explicitly permits its teacher leaf. The skeleton
uses no model tokens, but that says nothing about a leaf's cost or quality.

The API has bounded hop/candidate/text buffers and reports budget or refusal
states. It reads control-state files best-effort and can persist
`chain_last.txt` and JSON diagnostics under the governor directory.
A printed VERIFY event is a record, not an independent certificate.

[scripts/roe_reply.sh](../scripts/roe_reply.sh) wraps the native trace. Its
supported interface is a query with optional `--teacher`; it does not provide
the older Python formatter's `--md` option. See [reply diagnostics](REPLY_THINK.md).
