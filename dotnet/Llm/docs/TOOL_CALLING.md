# Tool-call parsing

Tool definitions and model-specific templates/parsers can turn generated
markup into structured calls. See
[parser implementations](../src/CNET.Llm.Tokenizers/ToolCallParsers/),
[ToolCallDetector](../src/CNET.Llm.Engine/ToolCallDetector.cs) and
[chat handler](../src/CNET.Llm.Server/Endpoints/ChatCompletionEndpoint.cs).

Parsing is not execution authorization. The host must allowlist names,
validate argument types/ranges, constrain resource use and obtain authority
for side effects. Schema-shaped output does not prove the action is safe
or its arguments correct.

Current server limitations are important:

- `tool_choice` is parsed but unused, including none/required/named choices.
- Streaming emits raw tool markup as content and supplies structured calls
  at the end; it is not a complete incremental tool-call-delta protocol.
- Constraint support is partial and output can be truncated.

See [CONSTRAINED_DECODING.md](CONSTRAINED_DECODING.md) and
[SERVER.md](SERVER.md). Do not rely on an ignored `tool_choice=none`
field as a security boundary. Historical integration examples are retained
in the [archive](../../../docs/MAINTENANCE.md).
