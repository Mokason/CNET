using System.Security.Cryptography;
using System.Text.Json;

namespace CnetControlPlane.Learning;

public sealed record LearningDataset(string Id, string Authority, string? SymbolVocabularySha256 = null);

/// <summary>Owner authority, not a worker proposal. Exact bytes are bound to the ledger.</summary>
public sealed class LearningPolicy
{
    private LearningPolicy() { }

    public string PolicyId { get; private init; } = string.Empty;
    public string Sha256 { get; private init; } = string.Empty;
    public bool Enabled { get; private init; }
    public IReadOnlyList<LearningDataset> Datasets { get; private init; } = Array.Empty<LearningDataset>();
    public int AttemptsPerHour { get; private init; }
    public int PromotionsPerDay { get; private init; }
    public int MaxAttemptsPerSource { get; private init; }
    public int MaxJobs { get; private init; }
    public int MaxStorageMiB { get; private init; }
    public int WorkerSeconds { get; private init; }
    public int WorkerMemoryMiB { get; private init; }
    public int TickSeconds { get; private init; }
    public int ProbationProbes { get; private init; }
    public int MaxProbeGapSeconds { get; private init; }
    public int MaxRunSeconds { get; private init; }
    public bool AllocatorEnabled { get; private init; }

    public void RequireEnabled()
    {
        if (!Enabled) throw new InvalidOperationException("learning_policy_disabled");
    }

    public static bool IsId(string? value) => value is { Length: >= 1 and <= 31 }
        && value[0] is >= 'a' and <= 'z'
        && value.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '_');

    internal static Dictionary<string, JsonElement> Fields(JsonElement element, params string[] required)
    {
        if (element.ValueKind != JsonValueKind.Object) throw new ArgumentException("learning_json_object_required");
        var result = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        foreach (var field in element.EnumerateObject())
            if (!result.TryAdd(field.Name, field.Value)) throw new ArgumentException("learning_json_duplicate_field");
        if (!result.Keys.ToHashSet(StringComparer.Ordinal).SetEquals(required))
            throw new ArgumentException("learning_json_missing_or_unknown_field");
        return result;
    }

    internal static int Number(JsonElement value, int minimum, int maximum)
    {
        if (value.ValueKind != JsonValueKind.Number || !value.GetRawText().All(c => c is >= '0' and <= '9')
            || !value.TryGetInt32(out var number) || number < minimum || number > maximum)
            throw new ArgumentException("learning_integer_out_of_bounds");
        return number;
    }

    internal static string Text(JsonElement value) => value.ValueKind == JsonValueKind.String
        ? value.GetString()! : throw new ArgumentException("learning_string_required");

    internal static bool Boolean(JsonElement value) => value.ValueKind switch
    {
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        _ => throw new ArgumentException("learning_boolean_required"),
    };

    /// <summary>Parses and hashes one private bounded snapshot, never the caller's live buffer.</summary>
    public static LearningPolicy Parse(byte[] bytes)
    {
        if (bytes.Length is < 1 or > 16384) throw new ArgumentException("learning_policy_size");
        var snapshot = (byte[])bytes.Clone();
        try
        {
            using var document = JsonDocument.Parse(snapshot, new JsonDocumentOptions { MaxDepth = 4 });
            var f = Fields(document.RootElement, "schema_version", "enabled", "policy_id", "datasets",
                "attempts_per_hour", "promotions_per_day", "max_attempts_per_source", "max_jobs", "max_storage_mib",
                "worker_seconds", "worker_memory_mib", "tick_seconds", "probation_probes", "max_probe_gap_seconds",
                "max_run_seconds", "allocator_enabled");
            _ = Number(f["schema_version"], 1, 1);
            var id = Text(f["policy_id"]);
            if (!IsId(id)) throw new ArgumentException("learning_policy_id");
            if (f["datasets"].ValueKind != JsonValueKind.Array || f["datasets"].GetArrayLength() is < 1 or > 16)
                throw new ArgumentException("learning_dataset_count");
            var datasets = new List<LearningDataset>();
            foreach (var item in f["datasets"].EnumerateArray())
            {
                var symbolic = item.ValueKind == JsonValueKind.Object && item.TryGetProperty("symbol_vocabulary_sha256", out _);
                var d = symbolic ? Fields(item, "id", "authority", "symbol_vocabulary_sha256") : Fields(item, "id", "authority");
                var name = Text(d["id"]);
                var authority = Text(d["authority"]);
                if (!IsId(name) || authority is not ("verified_tool" or "user_correction")
                    || datasets.Any(x => x.Id == name)) throw new ArgumentException("learning_dataset_authority");
                var vocabulary = symbolic ? Text(d["symbol_vocabulary_sha256"]) : null;
                if (vocabulary is not null && !IsHash(vocabulary)) throw new ArgumentException("learning_symbol_vocabulary_hash");
                datasets.Add(new LearningDataset(name, authority, vocabulary));
            }
            var tick = Number(f["tick_seconds"], 1, 3600);
            return new LearningPolicy
            {
                PolicyId = id, Sha256 = Convert.ToHexString(SHA256.HashData(snapshot)).ToLowerInvariant(),
                Enabled = Boolean(f["enabled"]), Datasets = datasets.AsReadOnly(),
                AttemptsPerHour = Number(f["attempts_per_hour"], 1, 32),
                PromotionsPerDay = Number(f["promotions_per_day"], 1, 30),
                MaxAttemptsPerSource = Number(f["max_attempts_per_source"], 1, 8),
                MaxJobs = Number(f["max_jobs"], 16, 4096),
                MaxStorageMiB = Number(f["max_storage_mib"], 16, 4096),
                WorkerSeconds = Number(f["worker_seconds"], 1, 120),
                WorkerMemoryMiB = Number(f["worker_memory_mib"], 64, 2048), TickSeconds = tick,
                ProbationProbes = Number(f["probation_probes"], 2, 32),
                MaxProbeGapSeconds = Number(f["max_probe_gap_seconds"], tick, 7200),
                MaxRunSeconds = Number(f["max_run_seconds"], 1, 604800),
                AllocatorEnabled = Boolean(f["allocator_enabled"]),
            };
        }
        catch (JsonException ex) { throw new ArgumentException("learning_policy_json_invalid", ex); }
    }

    internal static bool IsHash(string? text) => text is { Length: 64 }
        && text.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');
}
