using System.Buffers.Binary;
using System.Text;
using CnetControlPlane.Acceptance;

namespace CnetControlPlane.Tests;

public class RealModelAcceptanceTests
{
    private static byte[] GgufString(string value)
    {
        var encoded = Encoding.UTF8.GetBytes(value);
        var buf = new byte[8 + encoded.Length];
        BinaryPrimitives.WriteUInt64LittleEndian(buf, (ulong)encoded.Length);
        encoded.CopyTo(buf, 8);
        return buf;
    }

    private static void WriteTinyGguf(string path, int[] tensorTypes, string architecture = "qwen35")
    {
        using var ms = new MemoryStream();
        using var bw = new BinaryWriter(ms);
        bw.Write(Encoding.ASCII.GetBytes("GGUF"));
        bw.Write(3); // version
        bw.Write((ulong)tensorTypes.Length);
        bw.Write((ulong)2); // metadata count
        bw.Write(GgufString("general.architecture"));
        bw.Write(8); // string
        bw.Write(GgufString(architecture));
        bw.Write(GgufString("general.file_type"));
        bw.Write(4); // uint32
        bw.Write(36u);
        for (var index = 0; index < tensorTypes.Length; index++)
        {
            bw.Write(GgufString($"tensor.{index}"));
            bw.Write(1); // dimensions
            bw.Write((ulong)32);
            bw.Write(tensorTypes[index]);
            bw.Write((ulong)(index * 32));
        }
        File.WriteAllBytes(path, ms.ToArray());
    }

    [Fact]
    public void Gguf_and_qgkp_magic_are_enforced()
    {
        var root = Directory.CreateTempSubdirectory("cnet-magic-").FullName;
        try
        {
            var gguf = Path.Combine(root, "model.gguf");
            var qgkp = Path.Combine(root, "model.qgkp");
            var bad = Path.Combine(root, "bad.bin");
            File.WriteAllBytes(gguf, "GGUFpayload"u8.ToArray());
            File.WriteAllBytes(qgkp, "QGKPpayload"u8.ToArray());
            File.WriteAllBytes(bad, "NOPEpayload"u8.ToArray());
            RealModelAcceptance.RequireMagic(gguf, "GGUF"u8);
            RealModelAcceptance.RequireMagic(qgkp, "QGKP"u8);
            Assert.Throws<ArgumentException>(() => RealModelAcceptance.RequireMagic(bad, "GGUF"u8));
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void Top_logprob_entropy_is_normalized()
    {
        var peaked = new List<Dictionary<string, object?>>
        {
            new() { ["id"] = 1, ["logprob"] = Math.Log(0.99) },
            new() { ["id"] = 2, ["logprob"] = Math.Log(0.01) },
        };
        var flat = new List<Dictionary<string, object?>>
        {
            new() { ["id"] = 1, ["logprob"] = Math.Log(0.5) },
            new() { ["id"] = 2, ["logprob"] = Math.Log(0.5) },
        };
        Assert.True(RealModelAcceptance.NormalizedEntropy(peaked) < 0.1);
        Assert.True(RealModelAcceptance.NormalizedEntropy(flat) > 0.99);
    }

    [Fact]
    public void Distribution_metrics_and_recovery_curve_use_real_token_ids()
    {
        var reference = new List<Dictionary<string, object?>>
        {
            new() { ["id"] = 7, ["logprob"] = Math.Log(0.8) },
            new() { ["id"] = 9, ["logprob"] = Math.Log(0.2) },
        };
        var candidate = new List<Dictionary<string, object?>>
        {
            new() { ["id"] = 7, ["logprob"] = Math.Log(0.5) },
            new() { ["id"] = 11, ["logprob"] = Math.Log(0.5) },
        };
        var metrics = RealModelAcceptance.DistributionMetrics(reference, candidate);
        Assert.True(Convert.ToDouble(metrics["residual_l2_norm"]) > 0);
        var curve = (List<Dictionary<string, object?>>)metrics["recovery_curve"]!;
        Assert.Equal(new[] { 0.0, 0.25, 0.5, 0.75, 1.0 }, curve.Select(p => Convert.ToDouble(p["alpha"])));
        Assert.True(Convert.ToDouble(curve[0]["residual_l2_norm"]) > Convert.ToDouble(curve[^1]["residual_l2_norm"]));
        Assert.Equal(0.0, Convert.ToDouble(curve[^1]["residual_l2_norm"]), 7);
    }

    [Fact]
    public void Gguf_inspection_reports_embedded_quantization_not_filename()
    {
        var root = Directory.CreateTempSubdirectory("cnet-gguf-").FullName;
        try
        {
            var model = Path.Combine(root, "pretends-to-be-Q8_0.gguf");
            WriteTinyGguf(model, [34, 34, 12, 0]);
            var inspection = RealModelAcceptance.InspectGguf(model);
            Assert.Equal("qwen35", inspection["architecture"]);
            Assert.Equal(4L, Convert.ToInt64(inspection["tensor_count"]));
            var types = (Dictionary<string, int>)inspection["tensor_types"]!;
            Assert.Equal(1, types["F32"]);
            Assert.Equal(1, types["Q4_K"]);
            Assert.Equal(2, types["TQ1_0"]);
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void Quantization_diagnosis_flags_full_model_ternarization()
    {
        var reference = new Dictionary<string, object?>
        {
            ["tensor_count"] = 10,
            ["tensor_types"] = new Dictionary<string, int> { ["TQ1_0"] = 1, ["Q4_K"] = 9 },
        };
        var candidate = new Dictionary<string, object?>
        {
            ["tensor_count"] = 10,
            ["tensor_types"] = new Dictionary<string, int> { ["TQ1_0"] = 8, ["Q4_K"] = 2 },
        };
        var diagnosis = RealModelAcceptance.QuantizationDiagnosis(reference, candidate);
        Assert.True(Convert.ToBoolean(diagnosis["aggressive_ternarization"]));
        Assert.True(Convert.ToDouble(diagnosis["candidate_ternary_fraction"]) > 0.5);
    }

    [Fact]
    public void Structural_mismatch_quarantines_candidate()
    {
        var reasons = RealModelAcceptance.StructuralMismatchReasons(
            new Dictionary<string, object?> { ["architecture"] = "qwen35", ["tensor_count"] = 442 },
            new Dictionary<string, object?> { ["architecture"] = "qwen2", ["tensor_count"] = 441 });
        var admission = RealModelAcceptance.AdmitCandidate(
            new Dictionary<string, object?> { ["quality_pass_fraction"] = 1.0 },
            new Dictionary<string, object?> { ["quality_pass_fraction"] = 1.0 },
            maxQualityDelta: 0.0,
            structuralReasons: reasons);
        Assert.False(Convert.ToBoolean(admission["admitted"]));
        Assert.Contains("architecture_mismatch", (List<string>)admission["reasons"]!);
        Assert.Contains("tensor_count_mismatch", (List<string>)admission["reasons"]!);
    }

    [Fact]
    public void Candidate_is_quarantined_on_quality_regression()
    {
        var admission = RealModelAcceptance.AdmitCandidate(
            new Dictionary<string, object?> { ["quality_pass_fraction"] = 1.0 },
            new Dictionary<string, object?> { ["quality_pass_fraction"] = 0.0 },
            maxQualityDelta: 0.0);
        Assert.False(Convert.ToBoolean(admission["admitted"]));
        Assert.Equal("reference", admission["selected_role"]);
        Assert.Contains("quality_regression", (List<string>)admission["reasons"]!);
    }

    [Fact]
    public void Candidate_can_be_admitted_without_regression()
    {
        var admission = RealModelAcceptance.AdmitCandidate(
            new Dictionary<string, object?> { ["quality_pass_fraction"] = 2.0 / 3 },
            new Dictionary<string, object?> { ["quality_pass_fraction"] = 2.0 / 3 },
            maxQualityDelta: 0.0);
        Assert.True(Convert.ToBoolean(admission["admitted"]));
        Assert.Equal("candidate", admission["selected_role"]);
    }
}
