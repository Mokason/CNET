using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>A typed relation from one forest branch to another branch by name.</summary>
public readonly record struct CceForestConnection(string Name, CceConnectionType Type);

public enum CceBlockKind
{
    Linear = 0,
    Patch = 1,
    Depthwise = 2,
    LinearHead = 3,
}

/// <summary>Summary of one block inside a CCE branch cascade.</summary>
public sealed record CceBlockSummary(int Index, CceBlockKind Kind, int InputDim, int OutputDim)
{
    public long ParameterCount => Kind switch
    {
        CceBlockKind.Patch => checked((long)InputDim * OutputDim),
        _ => checked((long)InputDim * OutputDim + OutputDim),
    };
}

/// <summary>Summary of one branch inside a CCE forest.</summary>
public sealed record CceBranchSummary(
    int Index,
    string Name,
    IReadOnlyList<CceForestConnection> Connections,
    IReadOnlyList<CceBlockSummary> Blocks)
{
    public long ParameterCount
    {
        get
        {
            long total = 0;
            for (int i = 0; i < Blocks.Count; i++)
                total = checked(total + Blocks[i].ParameterCount);
            return total;
        }
    }
}

/// <summary>
/// Lightweight managed summary of a CCE forest. This is intentionally structural metadata,
/// not a PyTorch-style parameter tree.
/// </summary>
public sealed record CceForestSummary(IReadOnlyList<CceBranchSummary> Branches)
{
    public int BranchCount => Branches.Count;

    public override string ToString()
    {
        var sb = new StringBuilder();
        sb.Append("CceForest: ").Append(BranchCount).Append(" branch");
        if (BranchCount != 1) sb.Append('s');
        for (int i = 0; i < Branches.Count; i++)
        {
            var branch = Branches[i];
            sb.AppendLine();
            sb.Append("  [").Append(branch.Index).Append("] ").Append(branch.Name)
                .Append(" params=").Append(branch.ParameterCount);
            for (int b = 0; b < branch.Blocks.Count; b++)
            {
                var block = branch.Blocks[b];
                sb.AppendLine();
                sb.Append("      block[").Append(block.Index).Append("] ")
                    .Append(block.Kind).Append(' ')
                    .Append(block.InputDim).Append(" -> ").Append(block.OutputDim)
                    .Append(" params=").Append(block.ParameterCount);
            }
            for (int c = 0; c < branch.Connections.Count; c++)
            {
                var edge = branch.Connections[c];
                sb.AppendLine();
                sb.Append("      --").Append(edge.Type).Append("--> ").Append(edge.Name);
            }
        }
        return sb.ToString();
    }
}

/// <summary>
/// Safe-handle wrapper over a native cce_forest*. A forest is the model-definition unit:
/// a set of archive-backed branches plus typed composition relations between them.
/// This replaces passing raw IntPtr into <see cref="CceModel.Add(CceForest, string)"/>.
/// </summary>
public sealed class CceForest : SafeHandle
{
    private bool _owns = true;
    private IDisposable? _lifetimeOwner;  // e.g. the SupraA2A that owns the native specialists; keeps it alive while this forest is referenced

    private CceForest() : base(IntPtr.Zero, ownsHandle: true) { }

    public override bool IsInvalid => handle == IntPtr.Zero;

    internal IntPtr DangerousHandle => handle;

    /// <summary>
    /// Attach an owner (e.g. CceSupraA2A) so that this forest keeps the owner alive (fixes borrowed lifetime).
    /// The owner will not be disposed until this forest is.
    /// </summary>
    internal void AttachLifetimeOwner(IDisposable owner)
    {
        _lifetimeOwner = owner;
    }

    /// <summary>
    /// For borrowed handles (e.g. from Supra packed 1.6-bit), set owns=false so Dispose is a no-op on the native resource.
    /// </summary>
    internal void SetOwnership(bool owns)
    {
        _owns = owns;
    }

    protected override bool ReleaseHandle()
    {
        if (!_owns) 
        {
            _lifetimeOwner = null; // allow owner to be collected if no other refs
            return true;
        }
        if (handle != IntPtr.Zero)
        {
            CceNative.CceForestClose(handle);
            handle = IntPtr.Zero;
        }
        _lifetimeOwner = null;
        return true;
    }

    /// <summary>Opens (or creates) a forest backed by a .cce archive on disk.</summary>
    public static CceForest Open(string archivePath, int maxBranches = 64)
    {
        if (string.IsNullOrWhiteSpace(archivePath)) throw new ArgumentException("Path required", nameof(archivePath));
        var rc = CceNative.CceForestOpen(out IntPtr h, archivePath, maxBranches);
        if (rc != CceNative.CceResult.Ok || h == IntPtr.Zero)
            throw new InvalidOperationException($"CceForest.Open failed: {rc}");
        var f = new CceForest();
        f.SetHandle(h);
        f.SetOwnership(true);
        return f;
    }

    /// <summary>
    /// Wrap an existing native forest handle (e.g. obtained from CceSupraA2A.GetSpecialistsForest() for 1.6-bit packed models).
    /// owns=false: the wrapper will not release the native resource (lifetime owned by SupraA2A or other). Do not call Dispose on it.
    /// This is the key bridge to use packed Supra 1.6-bit specialists with Router, Planner, Contracts, Perceptual leaves and Glyph habitat composition from .NET.
    /// </summary>
    internal static CceForest FromNativeHandle(IntPtr nativeHandle, bool owns)
    {
        if (nativeHandle == IntPtr.Zero)
            throw new ArgumentNullException(nameof(nativeHandle));
        var f = new CceForest();
        f.SetHandle(nativeHandle);
        f.SetOwnership(owns);
        return f;
    }

    public int BranchCount => CceNative.CceForestBranchCount(handle);

    public string BranchName(int index)
    {
        var buf = new byte[64];
        var rc = CceNative.CceForestBranchName(handle, index, buf, buf.Length);
        if (rc != CceNative.CceResult.Ok)
            throw new ArgumentOutOfRangeException(nameof(index), $"branch_name failed: {rc}");
        int n = Array.IndexOf(buf, (byte)0);
        return Encoding.ASCII.GetString(buf, 0, n < 0 ? buf.Length : n);
    }

    /// <summary>
    /// Adds a named two-block linear specialist branch: input -> hidden -> raw output head.
    /// This is the compact model-definition path for ordinary CCE classifiers/regressors.
    /// </summary>
    public int AddLinearBranch(string name, int inputDim, int hiddenDim, int outputDim, float initScale = 0.01f)
    {
        if (string.IsNullOrWhiteSpace(name)) throw new ArgumentException("Branch name is required", nameof(name));
        if (inputDim <= 0) throw new ArgumentOutOfRangeException(nameof(inputDim));
        if (hiddenDim <= 0) throw new ArgumentOutOfRangeException(nameof(hiddenDim));
        if (outputDim <= 0) throw new ArgumentOutOfRangeException(nameof(outputDim));

        var rc = CceNative.CceForestAddLinearBranch(handle, name, inputDim, hiddenDim, outputDim, initScale, out int idx);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"AddLinearBranch failed: {rc}");
        return idx;
    }

    /// <summary>
    /// Adds a named patch specialist branch: flattened patch -> patch contract -> hidden -> raw output head.
    /// The input vector length for routed inference is <c>patchSize * patchSize * channels</c>.
    /// </summary>
    public int AddPatchBranch(string name, int patchSize, int stride, int channels, int hiddenDim, int outputDim, float initScale = 0.01f)
    {
        if (string.IsNullOrWhiteSpace(name)) throw new ArgumentException("Branch name is required", nameof(name));
        if (patchSize <= 0) throw new ArgumentOutOfRangeException(nameof(patchSize));
        if (stride <= 0) throw new ArgumentOutOfRangeException(nameof(stride));
        if (channels <= 0) throw new ArgumentOutOfRangeException(nameof(channels));
        if (hiddenDim <= 0) throw new ArgumentOutOfRangeException(nameof(hiddenDim));
        if (outputDim <= 0) throw new ArgumentOutOfRangeException(nameof(outputDim));

        var rc = CceNative.CceForestAddPatchBranch(handle, name, patchSize, stride, channels, hiddenDim, outputDim, initScale, out int idx);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"AddPatchBranch failed: {rc}");
        return idx;
    }

    internal int AddCascadeBranch(string name, CceCascadeBuilder builder)
    {
        if (string.IsNullOrWhiteSpace(name)) throw new ArgumentException("Branch name is required", nameof(name));
        ArgumentNullException.ThrowIfNull(builder);
        builder.ThrowIfInvalid();

        var rc = CceNative.CceForestAddCascadeBranch(handle, builder.DangerousHandle, name, out int idx);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"AddCascadeBranch failed: {rc}");
        builder.MarkMoved();
        return idx;
    }

    /// <summary>Connect two branches with a typed composition relation.</summary>
    public void Connect(int fromBranch, int toBranch, CceConnectionType type)
    {
        var rc = CceNative.CceForestConnect(handle, fromBranch, toBranch, (int)type);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Connect failed: {rc}");
    }

    /// <summary>Merge another forest's branches into this one (with an optional name prefix).</summary>
    public int Merge(CceForest src, string namePrefix = "")
    {
        ArgumentNullException.ThrowIfNull(src);
        int n = CceNative.CceForestMerge(handle, src.handle, namePrefix ?? "");
        if (n < 0) throw new InvalidOperationException($"Merge failed: {n}");
        return n;
    }

    public void SetDiffMode(CceDiffMode mode)
    {
        var rc = CceNative.CceForestSetDiffMode(handle, (CceNative.CceDiffMode)mode);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"SetDiffMode failed: {rc}");
    }

    /// <summary>
    /// Seals the forest. After sealing, no more branches can be added, and forward/infer
    /// operations can use stable zero-copy views into the memory-mapped archive.
    /// This is the key for maximum performance low-latency inference with many specialists.
    /// </summary>
    public void Seal()
    {
        var rc = CceNative.CceForestSeal(handle);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Seal failed: {rc}");
        _sealed = true;
    }

    /// <summary>
    /// Whether the forest has been sealed (zero-copy views are safe).
    /// </summary>
    public bool IsSealed
    {
        get
        {
            // We don't have a direct getter; track it or ask native. For now use a heuristic via forward behavior,
            // but expose a simple managed flag after Seal().
            // Better: add native accessor later. For this, we'll track locally.
            // Since native struct has it, for simplicity expose after call.
            return _sealed;
        }
    }

    private bool _sealed;

    /// <summary>
    /// Raw forward through a specific branch (bypasses model-level routing).
    /// After Seal(), this can be zero-copy / very low latency.
    /// </summary>
    public float[] ForwardBranch(int branchIndex, ReadOnlySpan<float> input, int outputDim)
    {
        if (input.IsEmpty) throw new ArgumentException("Input required");
        if (outputDim <= 0) throw new ArgumentOutOfRangeException(nameof(outputDim));

        float[] inArr = input.ToArray();
        float[] outArr = new float[outputDim];
        var rc = CceNative.CceForestForward(handle, branchIndex, inArr, inArr.Length, outArr, outputDim, out int actual);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"ForwardBranch failed: {rc}");

        if (actual != outputDim)
            Array.Resize(ref outArr, actual);
        return outArr;
    }

    /// <summary>
    /// Import weights + bias for a linear branch that was previously added (e.g. via AddLinearBranch or AddMlpBranch).
    /// Allows "pre-train a tiny specialist head in PyTorch/NumPy/elsewhere as float arrays and drop it into CCE".
    /// weights: row-major [outDim x inDim] for the head (or last linear).
    /// </summary>
    public void SetLinearBranchWeights(int branchIndex, float[] weights, int inDim, int outDim, float[] bias)
    {
        if (weights == null || bias == null) throw new ArgumentNullException();
        var rc = CceNative.CceForestSetBranchLinearWeights(handle, branchIndex, weights, inDim, outDim, bias, bias.Length);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"SetLinearBranchWeights failed: {rc}");
    }

    /// <summary>Overrides differentiation mode for one branch.</summary>
    public void SetBranchDiffMode(int branchIndex, CceDiffMode mode)
    {
        var rc = CceNative.CceForestSetBranchDiffMode(handle, branchIndex, (CceNative.CceDiffMode)mode);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"SetBranchDiffMode failed: {rc}");
    }

    /// <summary>Overrides exact-tail length for one branch when using hybrid differentiation.</summary>
    public void SetBranchExactTailLength(int branchIndex, int length)
    {
        if (length < 0) throw new ArgumentOutOfRangeException(nameof(length));
        var rc = CceNative.CceForestSetBranchExactTailLength(handle, branchIndex, length);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"SetBranchExactTailLength failed: {rc}");
    }

    /// <summary>Returns typed composition relations from a branch to other branch names.</summary>
    public IReadOnlyList<CceForestConnection> GetConnections(int branchIndex)
    {
        const int maxConnections = 8;
        const int nameWidth = 64;
        var names = new byte[maxConnections * nameWidth];
        var types = new int[maxConnections];
        int count = 0;
        var rc = CceNative.CceForestGetConnections(handle, branchIndex, names, types, ref count, maxConnections);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"GetConnections failed: {rc}");

        var result = new CceForestConnection[count];
        for (int i = 0; i < count; i++)
        {
            int offset = i * nameWidth;
            int len = 0;
            while (len < nameWidth && names[offset + len] != 0) len++;
            string name = Encoding.ASCII.GetString(names, offset, len);
            result[i] = new CceForestConnection(name, (CceConnectionType)types[i]);
        }
        return result;
    }

    /// <summary>Returns block type and shape metadata for one branch cascade.</summary>
    public IReadOnlyList<CceBlockSummary> GetBlocks(int branchIndex)
    {
        const int maxBlocks = 64;
        var types = new int[maxBlocks];
        var inputDims = new int[maxBlocks];
        var outputDims = new int[maxBlocks];
        int count = 0;
        var rc = CceNative.CceForestGetBranchBlocks(handle, branchIndex, types, inputDims, outputDims, ref count, maxBlocks);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"GetBlocks failed: {rc}");

        var result = new CceBlockSummary[count];
        for (int i = 0; i < count; i++)
            result[i] = new CceBlockSummary(i, (CceBlockKind)types[i], inputDims[i], outputDims[i]);
        return result;
    }

    /// <summary>
    /// Returns a lightweight managed description of branches and typed composition relations.
    /// </summary>
    public CceForestSummary Describe()
    {
        int count = BranchCount;
        var branches = new CceBranchSummary[count];
        for (int i = 0; i < count; i++)
        {
            branches[i] = new CceBranchSummary(i, BranchName(i), GetConnections(i), GetBlocks(i));
        }
        return new CceForestSummary(branches);
    }

    /// <summary>
    /// Convenience: adds a classic  input -> hidden -> head  specialist (2-block linear cascade).
    /// This is the most common "single specialist" pattern for classification/regression heads.
    /// </summary>
    public int AddMlpBranch(string name, int inputDim, int hiddenDim, int outputDim, float initScale = 0.01f)
    {
        return AddLinearBranch(name, inputDim, hiddenDim, outputDim, initScale);
    }

    /// <summary>
    /// Convenience: builds a small deeper cascade (linear stack) and adds it as a branch.
    /// Useful for slightly richer per-branch capacity while staying in the compositional model.
    /// </summary>
    public int AddDeepLinearBranch(string name, int inputDim, int[] hiddenDims, int outputDim, float initScale = 0.01f)
    {
        if (hiddenDims == null || hiddenDims.Length == 0)
            return AddLinearBranch(name, inputDim, Math.Max(4, inputDim / 2), outputDim, initScale);

        var b = CceCascadeBuilder.Create(hiddenDims.Length + 2);
        int prev = inputDim;
        foreach (var h in hiddenDims)
        {
            b.AddLinear(prev, h, initScale);
            prev = h;
        }
        b.AddLinearHead(prev, outputDim, initScale);
        // AddTo marks the builder moved; caller of AddDeep... does not dispose it.
        return b.AddTo(this, name);
    }

    protected override bool ReleaseHandle()
    {
        CceNative.CceForestClose(handle);
        return true;
    }
}
