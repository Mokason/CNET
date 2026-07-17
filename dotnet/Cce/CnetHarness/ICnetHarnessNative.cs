// Injectable native invoker. Managed unit tests substitute a fake
// implementation of this interface so that they can exercise option
// validation, UTF-8 projection, deterministic disposal, and error-path
// propagation without requiring libcnet_harness.so or a real GGUF model.

using System;

namespace CNET.Cce.CnetHarness;

internal interface ICnetHarnessNative
{
    int Open(in NativeConfig config, out IntPtr session);
    int Generate(IntPtr session, in NativeGenerateOptions options,
                 out IntPtr generation);
    int ProbeRoute(IntPtr session, string role,
                   CnetHarnessSamplingMode overrideMode,
                   ref NativeRouteInfo info);
    NativeGenerationLayout ReadGeneration(IntPtr generation);
    void GenerationFree(IntPtr generation);
    int Close(IntPtr session);
    string ErrorString(int status);
}

internal readonly record struct NativeGenerationLayout(
    string Text,
    uint PromptTokens,
    uint GeneratedTokens,
    double PromptMs,
    double GenerationMs,
    uint SelectedAdapter,
    float RouteUncertainty,
    CnetHarnessSamplingMode EffectiveSampling,
    bool AicimoOverride,
    float EffectiveTemperature,
    float EffectiveTopP,
    uint EffectiveTopK,
    float EffectiveMinP);
