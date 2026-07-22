using System.Runtime.Intrinsics;
using System.Runtime.Intrinsics.X86;
using CNET.Llm.Cpu.Kernels;
using Xunit;

namespace CNET.Llm.Tests.Unit.Cpu.Kernels;

/// <summary>
/// Verifies the VNNI-fused byte dot product against both the two-instruction
/// form it replaces and exact integer arithmetic.
///
/// The fusion is only safe because <c>maddubs</c>' int16 saturation is
/// unreachable for Q8_0 operands: one side is <c>abs()</c> of a Q8_0 value
/// (at most 128), the other a Q8_0 value (magnitude at most 127), so a pair sum
/// tops out at 128*127*2 = 32512, inside int16. These tests pin that boundary
/// rather than trusting the argument.
/// </summary>
public sealed unsafe class MatMulVnniTests
{
    /// <summary>The two-instruction sequence VNNI replaces.</summary>
    private static Vector256<int> Reference(Vector256<byte> u, Vector256<sbyte> s)
    {
        Vector256<short> prod = Avx2.MultiplyAddAdjacent(u, s);
        return Avx2.MultiplyAddAdjacent(prod, Vector256.Create((short)1));
    }

    /// <summary>Exact int32 grouping: four adjacent byte products per lane.</summary>
    private static int[] Exact(byte[] u, sbyte[] s)
    {
        var outp = new int[8];
        for (int lane = 0; lane < 8; lane++)
        {
            int sum = 0;
            for (int i = 0; i < 4; i++) sum += u[lane * 4 + i] * s[lane * 4 + i];
            outp[lane] = sum;
        }
        return outp;
    }

    private static (Vector256<byte> u, Vector256<sbyte> s) Load(byte[] u, sbyte[] s)
    {
        fixed (byte* up = u)
        fixed (sbyte* sp = s)
            return (Vector256.Load(up), Vector256.Load(sp));
    }

    [Fact]
    public void VnniIsActuallyExercisedHere()
    {
        // If this fails the rest of the file still passes but proves nothing
        // about the fused path, so state the precondition loudly.
        Assert.True(MatMul.VnniEnabled,
            "AvxVnni unavailable — the VNNI path is not being exercised on this machine");
    }

    [Fact]
    public void FusedMatchesReferenceAndExact_OverReachableRange()
    {
        var rng = new Random(97);
        var u = new byte[32];
        var s = new sbyte[32];

        for (int iter = 0; iter < 2000; iter++)
        {
            for (int i = 0; i < 32; i++)
            {
                u[i] = (byte)rng.Next(0, 129);        // abs(Q8_0) is 0..128
                s[i] = (sbyte)rng.Next(-127, 128);    // Q8_0 is -127..127
            }

            var (vu, vs) = Load(u, s);
            Vector256<int> fused = MatMul.DotBytesToInt32(vu, vs, Vector256.Create((short)1));
            Vector256<int> reference = Reference(vu, vs);
            int[] exact = Exact(u, s);

            for (int lane = 0; lane < 8; lane++)
            {
                Assert.Equal(reference[lane], fused[lane]);
                Assert.Equal(exact[lane], fused[lane]);
            }
        }
    }

    [Fact]
    public void FusedMatchesReferenceAndExact_AtSaturationBoundary()
    {
        // Worst reachable case: every product at the maximum magnitude, so each
        // lane's pair sums sit as close to int16 saturation as Q8_0 allows.
        var u = new byte[32];
        var s = new sbyte[32];
        foreach (sbyte extreme in new sbyte[] { 127, -127 })
        {
            for (int i = 0; i < 32; i++) { u[i] = 128; s[i] = extreme; }

            var (vu, vs) = Load(u, s);
            Vector256<int> fused = MatMul.DotBytesToInt32(vu, vs, Vector256.Create((short)1));
            Vector256<int> reference = Reference(vu, vs);
            int[] exact = Exact(u, s);

            for (int lane = 0; lane < 8; lane++)
            {
                Assert.Equal(exact[lane], fused[lane]);
                Assert.Equal(reference[lane], fused[lane]);
            }
            // 4 * 128 * 127 = 65024 per lane, and the int16 pair sums en route
            // are 128*127*2 = 32512 — just inside the 32767 ceiling.
            Assert.Equal(4 * 128 * extreme, fused[0]);
        }
    }

    /// <summary>
    /// Documents *why* the fusion needs the range argument: past the reachable
    /// range the two forms genuinely disagree, because maddubs saturates and
    /// VPDPBUSD does not. Q8_0 cannot produce these operands, but a future
    /// caller with wider inputs must not reuse this helper blindly.
    /// </summary>
    [Fact]
    public void OutsideReachableRange_FormsDivergeBecauseMaddubsSaturates()
    {
        Assert.True(MatMul.VnniEnabled, "requires VNNI to compare the two forms");

        var u = new byte[32];
        var s = new sbyte[32];
        for (int i = 0; i < 32; i++) { u[i] = 255; s[i] = 127; }   // pair sum 64770 > 32767

        var (vu, vs) = Load(u, s);
        Vector256<int> fused = MatMul.DotBytesToInt32(vu, vs, Vector256.Create((short)1));
        Vector256<int> reference = Reference(vu, vs);
        int[] exact = Exact(u, s);

        // VNNI stays exact; the maddubs form clamps at int16 and loses value.
        Assert.Equal(exact[0], fused[0]);
        Assert.NotEqual(reference[0], fused[0]);
        Assert.True(reference[0] < fused[0]);
    }
}
