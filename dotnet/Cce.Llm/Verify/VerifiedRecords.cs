using System.Text.Json;
using CNET.Cce.CnetHarness;
using CNET.Cce.Llm.Memory;

namespace CNET.Cce.Llm.Verify;

/// <summary>
/// A mechanical verifier: decides whether a candidate output is TRUE by
/// construction — parsing, checking, recomputing — never by opinion. What it
/// verifies it also canonicalizes, so records carry one normal form.
/// </summary>
public interface IVerifier
{
    string Name { get; }
    bool TryVerify(string candidate, out string verified);
}

/// <summary>
/// Verifies that a candidate contains one well-formed JSON document (fenced
/// or bare) and canonicalizes it. Born from live pain: minimax produced
/// syntactically broken quest JSON repeatedly — deep inside single chunks,
/// no seam involved — which no memory layer can fix but a parser can refuse.
/// </summary>
public sealed class JsonVerifier : IVerifier
{
    public string Name => "json";

    public bool TryVerify(string candidate, out string verified)
    {
        verified = "";
        string text = candidate;
        int fence = text.IndexOf("```json", StringComparison.Ordinal);
        if (fence >= 0)
        {
            int start = fence + "```json".Length;
            int end = text.IndexOf("```", start, StringComparison.Ordinal);
            if (end < 0) return false;              // unclosed fence: not verified
            text = text[start..end];
        }
        text = text.Trim();
        if (text.Length == 0 || (text[0] != '{' && text[0] != '[')) return false;

        try
        {
            using JsonDocument doc = JsonDocument.Parse(text);
            verified = JsonSerializer.Serialize(doc.RootElement,
                new JsonSerializerOptions { WriteIndented = true });
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }
}

/// <summary>Outcome of one distillation run.</summary>
/// <param name="Candidates">Samples drawn from the model.</param>
/// <param name="Verified">How many survived the verifier.</param>
/// <param name="SkillName">The noted skill (null when nothing verified).</param>
/// <param name="RecordPath">The written record (null when nothing verified).</param>
public sealed record DistillationReceipt(
    int Candidates, int Verified, string? SkillName, string? RecordPath);

/// <summary>
/// Generate-and-verify: sample the model, keep only what a mechanical
/// verifier accepts, and turn the survivor into a teaching record for the
/// gap lane's record teacher. The model is a proposal distribution here, not
/// a ceiling — junk never reaches training, so the certified specialist is
/// MORE reliable than the model it was distilled from. Rung 2 is rung 1 with
/// the verifier writing the record instead of the user.
/// </summary>
public sealed class VerifiedRecords(ICnetInferenceSession session, IVerifier verifier)
{
    /// <summary>Native seam, injectable for tests.</summary>
    internal Func<string, string, string, int>? NoteSkillOverride { get; set; }

    /// <summary>
    /// Samples <paramref name="samples"/> candidates for <paramref name="prompt"/>
    /// (varying seed, sampled — a proposal distribution needs variety), verifies
    /// each, and distills the first verified survivor into a record + k=1 gap.
    /// </summary>
    public DistillationReceipt Distill(string prompt, string inboxPath, string recordsDir,
                                       int samples = 4, uint maxTokens = 512)
    {
        ArgumentException.ThrowIfNullOrEmpty(prompt);
        ArgumentException.ThrowIfNullOrEmpty(inboxPath);
        ArgumentException.ThrowIfNullOrEmpty(recordsDir);
        ArgumentOutOfRangeException.ThrowIfLessThan(samples, 1);

        int drawn = 0;
        for (int i = 0; i < samples; i++)
        {
            CnetHarnessGenerationResult candidate = session.Generate(new CnetHarnessGenerateOptions
            {
                User = prompt,
                Role = "distill",
                MaxTokens = maxTokens,
                Seed = (uint)(9000 + i),           // vary the draw, keep it reproducible
                Sampling = CnetHarnessSamplingMode.Balanced,
            });
            drawn++;
            if (!verifier.TryVerify(candidate.Text, out string verified)) continue;

            // Survivor: the verified, canonicalized text is the record.
            // "#v2" marker: digits-only (tokenization-neutral), versions the format.
            string record = "#v2\n" + verified + "\n";
            string name = $"vrf_{Fnv8(record)}";
            Directory.CreateDirectory(recordsDir);
            File.WriteAllText(Path.Combine(recordsDir, $"skill_{name}.txt"), record);
            int rc = NoteSkillOverride is not null
                ? NoteSkillOverride(inboxPath, name, record)
                : CnetAutoLearnNative.NoteSkill(inboxPath, name, record, k: 1);
            if (rc != 0)
                return new DistillationReceipt(drawn, 1, null, null);
            return new DistillationReceipt(drawn, 1, name,
                Path.Combine(recordsDir, $"skill_{name}.txt"));
        }
        return new DistillationReceipt(drawn, 0, null, null);   // honest: nothing survived
    }

    private static string Fnv8(string text)
    {
        ulong h = 14695981039346656037UL;
        foreach (byte c in System.Text.Encoding.UTF8.GetBytes(text))
            h = (h ^ c) * 1099511628211UL;
        return ((uint)h).ToString("x8");
    }
}
