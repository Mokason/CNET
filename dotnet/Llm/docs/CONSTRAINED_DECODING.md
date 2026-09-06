# Constrained output

The engine's [constraints](../src/CNET.Llm.Engine/Constraints/) implement
JSON, a schema subset, regex automata and bounded grammar parsing.
They mask token candidates and advance parser state during generation.

This is structural assistance, not an unconditional guarantee of a complete,
valid or truthful answer. Max-token, context and stop exits can truncate
output. A caller must inspect completion status and independently validate
the final value before using it.

Schema compilation is partial: nonrecursive local `#/$defs` references
are supported, numeric/string bounds are not compiled, and required-property
tracking only covers bit positions below 64.
Grammar execution uses bounded stacks and ordered-choice semantics, not
an arbitrary unrestricted context-free grammar engine.

See [SchemaCompiler](../src/CNET.Llm.Engine/Constraints/Schema/SchemaCompiler.cs)
and the individual constraint tests before relying on a schema keyword.
Do not silently interpret ignored keywords as enforced security rules.
[Tool calls](TOOL_CALLING.md) additionally require name/argument authorization
and side-effect controls outside token generation.
