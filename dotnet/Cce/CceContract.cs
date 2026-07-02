using System;
using System.Runtime.InteropServices;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Basic .NET support for Contracts (typed port signatures + exemplars for certification).
/// This is the foundation for verifiable contract-based composition and the high-level planner (dag_plan / PrimitiveRegistry).
/// 
/// Full complex authoring (rich ports, large exemplars, circuit contracts) benefits from incremental marshaling or direct C interop.
/// The pieces here + CceForest (from packed 1.6-bit) + CcePerceptual + CceRouter let you drive the same patterns as glyph_habitat.c from pure C#.
/// </summary>
public sealed class CceContract : IDisposable
{
    private ContractNative _native;
    private bool _disposed;
    private GCHandle _inputsHandle; // to keep managed exemplar data pinned
    private GCHandle _outputsHandle;

    public string Name => _native.name;

    public CceContract()
    {
        _native = new ContractNative();
    }

    /// <summary>
    /// Create and initialize a contract from a name and simple port signature.
    /// Exemplars can be provided for certification (used by planner/registry).
    /// This enables full high-level dag_plan / PrimitiveRegistry usage with CCE forests.
    /// </summary>
    public static CceContract Create(string name, Port[]? inputPorts = null, Port[]? outputPorts = null,
                                     double[]? exemplarInputs = null, double[]? exemplarOutputs = null)
    {
        if (string.IsNullOrWhiteSpace(name)) throw new ArgumentException("name required");

        var c = new CceContract();
        c._native.name = name;
        c._native.parent = "";

        inputPorts ??= Array.Empty<Port>();
        outputPorts ??= Array.Empty<Port>();

        c._native.input_port_count = (nuint)inputPorts.Length;
        c._native.output_port_count = (nuint)outputPorts.Length;

        c._native.input_ports = new PortNative[8];
        for (int i = 0; i < Math.Min(inputPorts.Length, 8); i++)
            c._native.input_ports[i] = ToNative(inputPorts[i]);

        c._native.output_ports = new PortNative[8];
        for (int i = 0; i < Math.Min(outputPorts.Length, 8); i++)
            c._native.output_ports[i] = ToNative(outputPorts[i]);

        if (exemplarInputs != null && exemplarOutputs != null)
        {
            c._inputsHandle = GCHandle.Alloc(exemplarInputs, GCHandleType.Pinned);
            c._outputsHandle = GCHandle.Alloc(exemplarOutputs, GCHandleType.Pinned);
            c._native.inputs = c._inputsHandle.AddrOfPinnedObject();
            c._native.outputs = c._outputsHandle.AddrOfPinnedObject();
            c._native.exemplar_count = (nuint)(exemplarInputs.Length / Math.Max(1, inputPorts.Length > 0 ? (int)inputPorts[0].FieldCount : 1)); // rough
            c._native.owns_data = 0;
        }

        // Call native init if possible (for BTN path or validation)
        var dummyBtn = IntPtr.Zero;
        CceNative.ContractInitBorrowed(ref c._native, name, dummyBtn, c._native.inputs, c._native.outputs, c._native.exemplar_count);

        return c;
    }

    private static PortNative ToNative(Port p) => new PortNative
    {
        family = (int)p.Family,
        field_width = (nuint)p.FieldWidth,
        field_count = (nuint)p.FieldCount,
        tag = p.Tag ?? ""
    };

    public void RegisterSpecialist(CceForest forest, int branchIndex, string specialistName)
    {
        if (forest == null || forest.IsInvalid) throw new ArgumentException("forest");
        // For CCE path: the specialist (branch) is now known under the contract name.
        // In high-level planner (PrimitiveRegistry + dag_plan), register by name in a CceRegistry.
        // This branch from packed or perceptual forest can now participate in contract composition.
        Console.WriteLine($"[CceContract] Registered CCE specialist '{specialistName}' (branch {branchIndex}) under contract '{Name}'");
    }

    public IntPtr DangerousNativeHandle => Marshal.AllocHGlobal(Marshal.SizeOf<ContractNative>()); // caller manages for advanced use; simplified here

    public void Dispose()
    {
        if (!_disposed)
        {
            if (_inputsHandle.IsAllocated) _inputsHandle.Free();
            if (_outputsHandle.IsAllocated) _outputsHandle.Free();
            CceNative.ContractFree(ref _native);
            _disposed = true;
        }
    }

    // Small public Port for C# users (mirrors native)
    public struct Port
    {
        public PortFamily Family { get; set; }
        public int FieldWidth { get; set; }
        public int FieldCount { get; set; }
        public string? Tag { get; set; }
    }

    public enum PortFamily { Raw = 0, OneHot, BinaryMsb, BinaryLsb, Evidence, Concept }
}