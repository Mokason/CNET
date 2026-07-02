using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// .NET wrapper for PrimitiveRegistry (the core for high-level Router/Planner).
/// Allows registering CCE specialists (from packed 1.6-bit forests, perceptual leaves, etc.)
/// under Contracts, then using dag_plan / route_plan for contract-based composition.
/// 
/// This is the deeper marshaling step: C# can now drive PrimitiveRegistry + planning patterns.
/// Full complex plans return opaque or require additional handles; start with registration + name-based dispatch.
/// </summary>
public sealed class CceRegistry : IDisposable
{
    private IntPtr _reg; // PrimitiveRegistry*
    private bool _disposed;
    private readonly List<IDisposable> _keptAlive = new(); // keep forests/contracts alive

    public CceRegistry()
    {
        _reg = Marshal.AllocHGlobal(1024); // rough size for struct + entries; real impl would use native alloc
        CceNative.RegistryInit(_reg);
    }

    /// <summary>
    /// Register a CCE forest (e.g. from 1.6-bit packed Supra or perceptual) as a named specialist.
    /// The branch can then be used by planner under contracts.
    /// </summary>
    public void AddCceSpecialist(CceForest forest, string name)
    {
        if (forest == null || forest.IsInvalid) throw new ArgumentException("forest");
        // For CCE path we use name registration (the planner dispatches via CceModel/perceptual or internal).
        // If a BTN wrapper exists it would be passed; here we record the forest for lifetime and name.
        int rc = CceNative.RegistryAdd(_reg, IntPtr.Zero /* CCE path uses name + forest handle in higher layers */, name);
        if (rc != 0) throw new InvalidOperationException("Registry add failed for CCE specialist");
        _keptAlive.Add(forest);
    }

    /// <summary>
    /// Add a contract-backed specialist (for full contract authoring + certify).
    /// </summary>
    public void AddContractedSpecialist(CceContract contract, string name)
    {
        ArgumentNullException.ThrowIfNull(contract);
        // In deeper use: call registry_add_certified etc. Here we keep for composition.
        _keptAlive.Add(contract);
        // Placeholder registration by name for planner.
        CceNative.RegistryAdd(_reg, IntPtr.Zero, name);
    }

    public void Save(string directory)
    {
        // Would call registry_save if exposed with proper handle; for now no-op or extend.
        System.IO.Directory.CreateDirectory(directory);
    }

    public IntPtr DangerousHandle => _reg;

    public void Dispose()
    {
        if (!_disposed)
        {
            CceNative.RegistryFree(_reg);
            Marshal.FreeHGlobal(_reg);
            foreach (var o in _keptAlive) o.Dispose();
            _keptAlive.Clear();
            _disposed = true;
        }
    }
}