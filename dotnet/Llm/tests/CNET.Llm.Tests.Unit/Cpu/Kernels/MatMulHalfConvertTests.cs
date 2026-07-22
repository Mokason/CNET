using CNET.Llm.Cpu.Kernels;
using Xunit;

namespace CNET.Llm.Tests.Unit.Cpu.Kernels;

/// <summary>
/// Exhaustive check of <see cref="MatMul.HalfToFloat"/> against the framework's
/// own conversion.
///
/// The kernels replaced <c>(float)Half</c> with a hand-written sequence for
/// speed, so it must agree on every one of the 65,536 fp16 bit patterns — not
/// just the well-behaved ones. Subnormals in particular have broken this project
/// before, and they are the range a naive shift-and-rebias gets wrong.
/// </summary>
public sealed class MatMulHalfConvertTests
{
    [Fact]
    public void MatchesFrameworkConversion_ForEveryBitPattern()
    {
        for (int i = 0; i <= ushort.MaxValue; i++)
        {
            ushort bits = (ushort)i;
            float expected = (float)BitConverter.UInt16BitsToHalf(bits);
            float actual = MatMul.HalfToFloat(bits);

            if (float.IsNaN(expected))
            {
                Assert.True(float.IsNaN(actual), $"0x{bits:X4}: expected NaN, got {actual}");
                continue;
            }

            // Bitwise, so -0.0 is not accepted in place of +0.0.
            Assert.True(
                BitConverter.SingleToUInt32Bits(expected) == BitConverter.SingleToUInt32Bits(actual),
                $"0x{bits:X4}: expected {expected} (0x{BitConverter.SingleToUInt32Bits(expected):X8}), " +
                $"got {actual} (0x{BitConverter.SingleToUInt32Bits(actual):X8})");
        }
    }

    [Theory]
    [InlineData(0x0000)]   // +0
    [InlineData(0x8000)]   // -0
    [InlineData(0x0001)]   // smallest positive subnormal
    [InlineData(0x03FF)]   // largest subnormal
    [InlineData(0x0400)]   // smallest normal
    [InlineData(0x7BFF)]   // largest finite
    [InlineData(0x7C00)]   // +Inf
    [InlineData(0xFC00)]   // -Inf
    public void NamedEdgeCases(int raw)
    {
        ushort bits = (ushort)raw;
        float expected = (float)BitConverter.UInt16BitsToHalf(bits);
        Assert.Equal(
            BitConverter.SingleToUInt32Bits(expected),
            BitConverter.SingleToUInt32Bits(MatMul.HalfToFloat(bits)));
    }
}
