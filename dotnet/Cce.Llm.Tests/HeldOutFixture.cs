using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// Held-out capability fixture consumption for the .NET evaluators — the C#
/// counterpart of <c>include/cnet_heldout.h</c>, emitting the byte-identical
/// receipt lines that <c>tests/run_capability_cert.py</c> and
/// <c>tests/test_capability_fixture_causality.py</c> require.
/// </summary>
/// <remarks>
/// <para>
/// WHY THIS EXISTS. The capability runner exported <c>CNET_HELD_OUT_FIXTURE</c>
/// and no evaluator read it, so a declared held-out case could be edited freely
/// while the certificate stayed green. For the .NET capability there is a second
/// failure mode with the same signature: <c>dotnet test --no-restore</c> against
/// an unrestored project exits 0 having run zero tests. Requiring a receipt on
/// stdout closes both — a fixture that was not consumed, and an evaluator that
/// did not run, are indistinguishable from the runner's side and both fail.
/// </para>
/// <para>
/// Two modes, matching the C reader: with the variable unset every read returns
/// the caller's fallback and nothing is printed, so <c>make dotnet_cce_tests</c>
/// behaves exactly as before; with it set the fixture MUST load, MUST declare
/// this capability, and every read MUST resolve.
/// </para>
/// </remarks>
internal sealed class HeldOutFixture : IDisposable
{
    public const string EnvironmentVariable = "CNET_HELD_OUT_FIXTURE";

    private const int MaxBytes = 1024 * 1024;

    private readonly Dictionary<string, JsonElement> _cases = new(StringComparer.Ordinal);
    private readonly List<string> _order = new();
    private readonly Dictionary<string, int> _reads = new(StringComparer.Ordinal);
    private readonly Dictionary<string, bool> _verdicts = new(StringComparer.Ordinal);
    private JsonDocument? _document;

    private HeldOutFixture(bool required) => Required = required;

    /// <summary>True when the run declared a fixture and is therefore bound to it.</summary>
    public bool Required { get; }

    public string CapabilityId { get; private set; } = string.Empty;

    public string Sha256 { get; private set; } = string.Empty;

    /// <summary>Unresolved reads, unknown case ids, and wrong-typed values.</summary>
    public int Errors { get; private set; }

    /// <summary>
    /// Load the fixture named by <c>CNET_HELD_OUT_FIXTURE</c> and require it to
    /// declare <paramref name="capabilityId"/>. Returns a standalone instance
    /// when no fixture was requested. Throws when one was requested but is
    /// unusable — an evaluator must never silently fall back to its own cases.
    /// </summary>
    public static HeldOutFixture Open(string capabilityId)
    {
        ArgumentException.ThrowIfNullOrEmpty(capabilityId);
        string? path = Environment.GetEnvironmentVariable(EnvironmentVariable);
        if (string.IsNullOrEmpty(path))
        {
            return new HeldOutFixture(required: false);
        }

        var fixture = new HeldOutFixture(required: true);
        fixture.Load(path, capabilityId);
        return fixture;
    }

    private void Load(string path, string capabilityId)
    {
        byte[] bytes = File.ReadAllBytes(path);
        if (bytes.Length == 0 || bytes.Length > MaxBytes)
        {
            throw new InvalidOperationException(
                $"heldout: fixture {path} has an unusable length {bytes.Length}");
        }

        // Hash the exact bytes that were parsed, so the digest in the receipt
        // cannot drift from the content that drove the assertions.
        Sha256 = Convert.ToHexStringLower(SHA256.HashData(bytes));
        _document = JsonDocument.Parse(bytes);
        JsonElement root = _document.RootElement;
        if (root.ValueKind != JsonValueKind.Object)
        {
            throw new InvalidOperationException($"heldout: fixture {path} is not an object");
        }

        if (!root.TryGetProperty("capability_id", out JsonElement declared) ||
            declared.ValueKind != JsonValueKind.String)
        {
            throw new InvalidOperationException($"heldout: fixture {path} has no capability_id");
        }

        CapabilityId = declared.GetString() ?? string.Empty;
        if (!string.Equals(CapabilityId, capabilityId, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"heldout: fixture {path} declares capability {CapabilityId}, " +
                $"evaluator is {capabilityId}");
        }

        if (!root.TryGetProperty("cases", out JsonElement cases) ||
            cases.ValueKind != JsonValueKind.Array)
        {
            throw new InvalidOperationException($"heldout: fixture {path} has no cases array");
        }

        foreach (JsonElement element in cases.EnumerateArray())
        {
            if (element.ValueKind != JsonValueKind.Object ||
                !element.TryGetProperty("id", out JsonElement id) ||
                id.ValueKind != JsonValueKind.String ||
                string.IsNullOrEmpty(id.GetString()))
            {
                throw new InvalidOperationException(
                    $"heldout: fixture {path} has a case without a string id");
            }

            string caseId = id.GetString()!;
            if (!_cases.TryAdd(caseId, element))
            {
                throw new InvalidOperationException(
                    $"heldout: fixture {path} repeats case id {caseId}");
            }

            _order.Add(caseId);
        }

        if (_order.Count == 0)
        {
            throw new InvalidOperationException($"heldout: fixture {path} declares no cases");
        }
    }

    private bool TryValue(string caseId, string key, out JsonElement value)
    {
        value = default;
        if (!Required)
        {
            return false;
        }

        if (!_cases.TryGetValue(caseId, out JsonElement element))
        {
            Console.Error.WriteLine($"heldout: fixture has no case {caseId}");
            Errors++;
            return false;
        }

        if (!element.TryGetProperty(key, out value))
        {
            Console.Error.WriteLine($"heldout: case {caseId} has no key {key}");
            Errors++;
            return false;
        }

        _reads[caseId] = _reads.GetValueOrDefault(caseId) + 1;
        return true;
    }

    /// <summary>Read a string field. Standalone runs get the fallback untouched.</summary>
    public string Str(string caseId, string key, string fallback)
    {
        if (!TryValue(caseId, key, out JsonElement value))
        {
            return fallback;
        }

        if (value.ValueKind != JsonValueKind.String)
        {
            Console.Error.WriteLine($"heldout: case {caseId} key {key} is not a string");
            Errors++;
            return fallback;
        }

        return value.GetString() ?? fallback;
    }

    /// <summary>Read a numeric field. Standalone runs get the fallback untouched.</summary>
    public double Num(string caseId, string key, double fallback)
    {
        if (!TryValue(caseId, key, out JsonElement value))
        {
            return fallback;
        }

        if (value.ValueKind == JsonValueKind.True)
        {
            return 1.0;
        }

        if (value.ValueKind == JsonValueKind.False)
        {
            return 0.0;
        }

        if (value.ValueKind != JsonValueKind.Number || !value.TryGetDouble(out double number))
        {
            Console.Error.WriteLine($"heldout: case {caseId} key {key} is not a number");
            Errors++;
            return fallback;
        }

        return number;
    }

    /// <summary>Read an array's length. Standalone runs get the fallback untouched.</summary>
    public int ArrayLen(string caseId, string key, int fallback)
    {
        if (!TryValue(caseId, key, out JsonElement value))
        {
            return fallback;
        }

        if (value.ValueKind != JsonValueKind.Array)
        {
            Console.Error.WriteLine($"heldout: case {caseId} key {key} is not an array");
            Errors++;
            return fallback;
        }

        return value.GetArrayLength();
    }

    /// <summary>Record this case's verdict. A failure is never upgraded.</summary>
    public void Verdict(string caseId, bool ok)
    {
        if (!Required)
        {
            return;
        }

        if (!_cases.ContainsKey(caseId))
        {
            Console.Error.WriteLine($"heldout: verdict for unknown case {caseId}");
            Errors++;
            return;
        }

        _verdicts[caseId] = _verdicts.TryGetValue(caseId, out bool previous) ? previous && ok : ok;
    }

    /// <summary>
    /// Print the receipt and report whether the fixture was honoured. True in
    /// standalone mode (nothing printed) and in bound mode only when every
    /// declared case was read, every read resolved, and every verdict passed.
    /// </summary>
    public bool Finish()
    {
        if (!Required)
        {
            return true;
        }

        var receipt = new StringBuilder();
        int consumed = 0;
        int passed = 0;
        foreach (string caseId in _order)
        {
            int reads = _reads.GetValueOrDefault(caseId);
            receipt.Append(CultureInfo.InvariantCulture, $"HELDOUT_CASE id={caseId} reads={reads}\n");
            if (reads > 0)
            {
                consumed++;
            }

            bool ok = _verdicts.TryGetValue(caseId, out bool verdict) && verdict;
            if (ok)
            {
                passed++;
            }
            else
            {
                string state = _verdicts.ContainsKey(caseId) ? "failed" : "not_evaluated";
                receipt.Append(CultureInfo.InvariantCulture,
                    $"HELDOUT_CASE_FAIL id={caseId} verdict={state}\n");
            }
        }

        receipt.Append(CultureInfo.InvariantCulture,
            $"HELDOUT_FIXTURE capability={CapabilityId} sha256={Sha256} " +
            $"cases={_order.Count} consumed={consumed}\n");
        double metric = _order.Count == 0 ? 0.0 : (double)passed / _order.Count;
        receipt.Append(CultureInfo.InvariantCulture,
            $"HELDOUT_METRIC cases_passed={passed} cases_declared={_order.Count} " +
            $"metric={metric.ToString("F6", CultureInfo.InvariantCulture)} errors={Errors}\n");

        // The VSTest host multiplexes stdout; write the whole receipt in one go
        // so the runner's line-anchored patterns cannot be split by interleaving.
        Console.Out.Write(receipt.ToString());
        Console.Out.Flush();
        return Errors == 0 && consumed == _order.Count && passed == _order.Count;
    }

    public void Dispose()
    {
        _document?.Dispose();
        _document = null;
    }
}
