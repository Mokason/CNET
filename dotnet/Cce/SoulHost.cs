using System;
using System.Collections.Generic;
using System.Text;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Native CNB Oracle provenance. A descriptor records bounded identity and
/// intent; it does not claim that the Oracle is currently bound or admitted as
/// a runtime primitive.
/// </summary>
public sealed record OracleDescriptor(
    string Name,
    string Kind,
    ulong BehaviorDigest,
    ulong ArtifactDigest,
    ulong ContractDigest,
    ulong ConfigDigest,
    ulong RetrievalSnapshotDigest,
    ulong ToolchainDigest);

/// <summary>
/// Managed wrapper over the CNET soul_host shim: load a certified .cnb base,
/// query a unit's port sizes, run named certified units safely, route by real
/// typed ports, and read real (Laplace-smoothed) reliability. Returns actual
/// data — no faked "reliability high" strings.
/// </summary>
public sealed class SoulHost : IDisposable
{
    private IntPtr _handle;
    private bool _disposed;

    public SoulHost(string basePath, string? modelPath = null)
    {
        int rc = CceNative.SoulOpen(basePath, modelPath, out _handle);
        if (rc != 0 || _handle == IntPtr.Zero)
            throw new InvalidOperationException($"soul_open('{basePath}') failed: {rc}");
    }

    /// <summary>
    /// Snapshot the canonical roster admitted by native certification replay.
    /// No gap ledger, manifest, or filename convention is used as authority.
    /// </summary>
    public IReadOnlyList<string> Units()
    {
        Check();
        int count = CceNative.SoulUnitCount(_handle);
        if (count < 0) throw new InvalidOperationException($"soul_unit_count failed: {count}");
        var names = new string[count];
        for (int i = 0; i < count; ++i)
        {
            var buffer = new byte[256];
            int rc = CceNative.SoulUnitName(_handle, i, buffer, buffer.Length);
            if (rc != 0) throw new InvalidOperationException($"soul_unit_name({i}) failed: {rc}");
            int length = Array.IndexOf(buffer, (byte)0);
            if (length < 0) length = buffer.Length;
            names[i] = Encoding.UTF8.GetString(buffer, 0, length);
        }
        return names;
    }

    /// <summary>
    /// Snapshot evidence-carrying Oracle descriptors from the authoritative
    /// native base. This is provenance metadata, not runtime-admission state.
    /// </summary>
    public IReadOnlyList<OracleDescriptor> Oracles()
    {
        Check();
        int count = CceNative.SoulOracleCount(_handle);
        if (count < 0) throw new InvalidOperationException($"soul_oracle_count failed: {count}");
        var descriptors = new OracleDescriptor[count];
        for (int i = 0; i < count; ++i)
        {
            var nameBuffer = new byte[256];
            var kindBuffer = new byte[256];
            int rc = CceNative.SoulOracleName(_handle, i, nameBuffer, nameBuffer.Length);
            if (rc != 0) throw new InvalidOperationException($"soul_oracle_name({i}) failed: {rc}");
            rc = CceNative.SoulOracleKind(_handle, i, kindBuffer, kindBuffer.Length);
            if (rc != 0) throw new InvalidOperationException($"soul_oracle_kind({i}) failed: {rc}");
            rc = CceNative.SoulOracleIdentity(
                _handle, i,
                out ulong behavior, out ulong artifact, out ulong contract,
                out ulong config, out ulong retrieval, out ulong toolchain);
            if (rc != 0) throw new InvalidOperationException($"soul_oracle_identity({i}) failed: {rc}");
            descriptors[i] = new OracleDescriptor(
                DecodeAtom(nameBuffer), DecodeAtom(kindBuffer),
                behavior, artifact, contract, config, retrieval, toolchain);
        }
        return descriptors;
    }

    private static string DecodeAtom(byte[] buffer)
    {
        int length = Array.IndexOf(buffer, (byte)0);
        if (length < 0) length = buffer.Length;
        return Encoding.UTF8.GetString(buffer, 0, length);
    }

    /// <summary>(inputTotal, outputTotal) of a unit, or throws if absent.</summary>
    public (int In, int Out) UnitDims(string name)
    {
        Check();
        int rc = CceNative.SoulUnitDims(_handle, name, out int inT, out int outT);
        if (rc != 0) throw new InvalidOperationException($"unit '{name}' not found ({rc})");
        return (inT, outT);
    }

    /// <summary>Run a certified unit; returns its full output vector (correctly sized).</summary>
    public double[] RunUnit(string name, double[] input)
    {
        Check();
        var (_, outT) = UnitDims(name);
        var output = new double[outT];
        int written = CceNative.SoulRun(_handle, name, input, output, outT);
        if (written < 0) throw new InvalidOperationException($"soul_run('{name}') failed: {written}");
        return output;
    }

    /// <summary>Ordered top-k token indices per output field (the certified ranked answer).</summary>
    public int[] TopK(string name, int currentTokenIndex, int fieldWidth = 256, int fields = 3)
    {
        var input = new double[fieldWidth];
        input[currentTokenIndex] = 1.0;
        var output = RunUnit(name, input);
        var picks = new int[fields];
        for (int f = 0; f < fields; f++)
        {
            int best = 0;
            for (int j = 1; j < fieldWidth; j++)
                if (output[f * fieldWidth + j] > output[f * fieldWidth + best]) best = j;
            picks[f] = best;
        }
        return picks;
    }

    /// <summary>Route to the unit owning goalTag via real typed ports; returns its output.</summary>
    public double[] Route(string goalTag, double[] input, int outputSize = 768)
    {
        Check();
        var output = new double[outputSize];
        int written = CceNative.SoulRoute(_handle, goalTag, input, input.Length, output, outputSize);
        if (written < 0) throw new InvalidOperationException($"soul_route('{goalTag}') failed: {written}");
        return output;
    }

    /// <summary>Real Laplace-smoothed reliability (0..1); 0.5 fresh until executed.</summary>
    public double Reliability(string name)
    {
        Check();
        int milli = CceNative.SoulUnitReliabilityMilli(_handle, name);
        return milli < 0 ? -1.0 : milli / 1000.0;
    }

    private void Check()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(SoulHost));
    }

    public void Dispose()
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            CceNative.SoulClose(_handle);
            _handle = IntPtr.Zero;
        }
        _disposed = true;
    }
}
