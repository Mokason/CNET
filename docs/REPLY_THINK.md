# Reply diagnostics

The reply panel is a structured report of query, route, control state and
outcome. It is not a language model's private internal process, and rendering
a check name does not prove that check passed.

There are two separate implementations:

- [roe_reply.sh](../scripts/roe_reply.sh) wraps the native execution trace.
  It accepts a query and optional `--teacher`.
- [cnet_reply_think.py](../scripts/cnet_reply_think.py) formats diagnostics,
  including a Markdown panel; it may optionally obtain a front-door answer.

Use `make cnet_reply_think` for the formatter gate and
`make roe_chain_think` for the native path. Do not pass the old `--md`
example to the shell wrapper; it does not select the Python formatter.

The panel itself consumes no model tokens. An executed leaf may consume
tokens, and injected `--answer` / `--source` values remain caller-supplied.
Logs are useful diagnostics, not authenticated evidence or promotion authority.
See [trace semantics](CHAIN_OF_THOUGHT.md) and [continuity](CONTINUITY_WORKSPACE.md).
