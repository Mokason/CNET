// LibraryImport bindings for libcnet_harness.so. Source-generated on .NET 10
// for AOT and performance. Native structs mirror include/cnet_harness.h with
// abi_version+struct_size as the first two uint32_t fields.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.Marshalling;

namespace CNET.Cce.CnetHarness;

internal static class AbiConstants
{
    public const uint AbiVersion = 1u;
    public const uint OffloadAbiVersion = 1u;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeOffloadPolicy
{
    public uint AbiVersion;
    public uint StructSize;
    public int GpuLayerCount;
    public uint DeviceCount;
    public int Device0;
    public int Device1;
    public int Device2;
    public int Device3;
    public float TensorSplit0;
    public float TensorSplit1;
    public float TensorSplit2;
    public float TensorSplit3;
    public CnetHarnessSplitMode SplitMode;
    public uint OffloadKqv;
    public ulong MaxVramBytesPerDevice;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeOffloadInfo
{
    public uint AbiVersion;
    public uint StructSize;
    public int RequestedGpuLayers;
    public int AppliedGpuLayers;
    public int ModelLayerCount;
    public uint DeviceCount;
    public int Device0;
    public int Device1;
    public int Device2;
    public int Device3;
    public ulong VramBytes0;
    public ulong VramBytes1;
    public ulong VramBytes2;
    public ulong VramBytes3;
    public CnetHarnessSplitMode SplitMode;
    public uint OffloadKqv;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeConfig
{
    public uint AbiVersion;
    public uint StructSize;
    public IntPtr ModelId;
    public IntPtr ModelPath;
    public ulong ResourceMask;
    public ulong BudgetBytes;
    public int MainGpu;
    public uint NCtx;
    public uint NBatch;
    public uint NThreads;
    public uint AicimoNumOps;
    public uint AicimoBaseDim;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeGenerateOptions
{
    public uint AbiVersion;
    public uint StructSize;
    public IntPtr System;
    public IntPtr User;
    public IntPtr Role;
    public uint MaxTokens;
    public uint Seed;
    public CnetHarnessSamplingMode Sampling;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeGeneration
{
    public uint AbiVersion;
    public uint StructSize;
    public IntPtr Text;
    public uint PromptTokens;
    public uint GeneratedTokens;
    public double PromptMs;
    public double GenerationMs;
    public uint SelectedAdapter;
    public float RouteUncertainty;
    public CnetHarnessSamplingMode EffectiveSampling;
    public int AicimoOverride;
    public float EffectiveTemperature;
    public float EffectiveTopP;
    public uint EffectiveTopK;
    public float EffectiveMinP;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeRouteInfo
{
    public uint AbiVersion;
    public uint StructSize;
    public uint SelectedAdapter;
    public float RouteUncertainty;
    public CnetHarnessSamplingMode EffectiveSampling;
    public float EffectiveTemperature;
    public float EffectiveTopP;
    public uint EffectiveTopK;
    public float EffectiveMinP;
}

internal static partial class CnetHarnessNativeImports
{
    private const string LibraryName = "cnet_harness";

    static CnetHarnessNativeImports() => CnetHarnessLibraryResolver.EnsureRegistered();

    [LibraryImport(LibraryName, EntryPoint = "cnet_harness_open")]
    public static partial int Open(ref NativeConfig config, out IntPtr session);

    [LibraryImport(LibraryName, EntryPoint = "cnet_harness_open_with_offload")]
    public static partial int OpenWithOffload(ref NativeConfig config,
                                              ref NativeOffloadPolicy policy,
                                              out IntPtr session);

    [LibraryImport(LibraryName, EntryPoint = "cnet_harness_get_offload_info")]
    public static partial int GetOffloadInfo(IntPtr session,
                                             ref NativeOffloadInfo info);

    [LibraryImport(LibraryName, EntryPoint = "cnet_harness_generate")]
    public static partial int Generate(IntPtr session,
                                        ref NativeGenerateOptions options,
                                        out IntPtr generation);

    [LibraryImport(LibraryName, EntryPoint = "cnet_harness_count_tokens",
                   StringMarshalling = StringMarshalling.Utf8)]
    public static partial int CountTokens(IntPtr session, string text, out int count);

    [LibraryImport(LibraryName, EntryPoint = "cnet_harness_probe_route",
                   StringMarshalling = StringMarshalling.Utf8)]
    public static partial int ProbeRoute(IntPtr session, string role,
                                          CnetHarnessSamplingMode overrideMode,
                                          ref NativeRouteInfo info);

    [LibraryImport(LibraryName, EntryPoint = "cnet_harness_generation_free")]
    public static partial void GenerationFree(IntPtr generation);

    [LibraryImport(LibraryName, EntryPoint = "cnet_harness_close")]
    public static partial int Close(IntPtr session);

    [LibraryImport(LibraryName, EntryPoint = "cnet_harness_error_string")]
    public static partial IntPtr ErrorString(int status);
}

/// <summary>Default production invoker that P/Invokes libcnet_harness.so.</summary>
internal sealed class LibraryCnetHarnessNative : ICnetHarnessNative
{
    public int Open(in NativeConfig config, out IntPtr session)
    {
        NativeConfig local = config;
        return CnetHarnessNativeImports.Open(ref local, out session);
    }

    public int CountTokens(IntPtr session, string text, out int count)
        => CnetHarnessNativeImports.CountTokens(session, text, out count);

    public int OpenWithOffload(in NativeConfig config,
                               in NativeOffloadPolicy policy,
                               out IntPtr session)
    {
        NativeConfig configLocal = config;
        NativeOffloadPolicy policyLocal = policy;
        return CnetHarnessNativeImports.OpenWithOffload(
            ref configLocal, ref policyLocal, out session);
    }

    public int GetOffloadInfo(IntPtr session, ref NativeOffloadInfo info)
        => CnetHarnessNativeImports.GetOffloadInfo(session, ref info);

    public int Generate(IntPtr session, in NativeGenerateOptions options,
                        out IntPtr generation)
    {
        NativeGenerateOptions local = options;
        return CnetHarnessNativeImports.Generate(session, ref local, out generation);
    }

    public int ProbeRoute(IntPtr session, string role,
                          CnetHarnessSamplingMode overrideMode,
                          ref NativeRouteInfo info)
    {
        return CnetHarnessNativeImports.ProbeRoute(session, role, overrideMode, ref info);
    }

    public unsafe NativeGenerationLayout ReadGeneration(IntPtr generation)
    {
        if (generation == IntPtr.Zero)
        {
            throw new CnetHarnessException(CnetHarnessStatus.Internal,
                "native returned null generation pointer");
        }
        uint abiVersion = unchecked((uint)Marshal.ReadInt32(generation, 0));
        uint structSize = unchecked((uint)Marshal.ReadInt32(generation, sizeof(uint)));
        if (abiVersion != AbiConstants.AbiVersion ||
            structSize != (uint)Marshal.SizeOf<NativeGeneration>())
        {
            throw new CnetHarnessException(CnetHarnessStatus.InvalidState,
                $"native generation ABI mismatch: version={abiVersion}, size={structSize}");
        }
        NativeGeneration native = Unsafe.Read<NativeGeneration>((void *)generation);
        string text = native.Text == IntPtr.Zero
            ? string.Empty
            : Marshal.PtrToStringUTF8(native.Text) ?? string.Empty;
        return new NativeGenerationLayout(
            text,
            native.PromptTokens,
            native.GeneratedTokens,
            native.PromptMs,
            native.GenerationMs,
            native.SelectedAdapter,
            native.RouteUncertainty,
            native.EffectiveSampling,
            native.AicimoOverride != 0,
            native.EffectiveTemperature,
            native.EffectiveTopP,
            native.EffectiveTopK,
            native.EffectiveMinP);
    }

    public void GenerationFree(IntPtr generation)
        => CnetHarnessNativeImports.GenerationFree(generation);

    public int Close(IntPtr session)
        => CnetHarnessNativeImports.Close(session);

    public string ErrorString(int status)
    {
        IntPtr p = CnetHarnessNativeImports.ErrorString(status);
        return p == IntPtr.Zero
            ? "unknown"
            : Marshal.PtrToStringUTF8(p) ?? "unknown";
    }
}
