using System.Text.Json.Serialization;
using CNET.Llm.Cli.Commands;

namespace CNET.Llm.Cli;

/// <summary>
/// Source-generated JSON serializer context for CLI output types.
/// Enables Native AOT compilation by eliminating reflection-based serialization.
/// </summary>
[JsonSerializable(typeof(RunJsonResult))]
[JsonSourceGenerationOptions(
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    PropertyNamingPolicy = JsonKnownNamingPolicy.SnakeCaseLower)]
internal partial class CliJsonContext : JsonSerializerContext;
