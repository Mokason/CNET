using CNET.Llm.Core.Sampling;
using CNET.Llm.Engine.Samplers.StopConditions;
using Xunit;

namespace CNET.Llm.Tests.Unit.Engine.Samplers.StopConditions;

public class EosStopConditionTests
{
    [Fact]
    public void ShouldStop_EosToken_ReturnsStop()
    {
        var condition = new EosStopCondition(eosTokenId: 2);

        var result = condition.ShouldStop(tokenId: 2, generatedTokens: [2], decodedText: "");

        Assert.Equal(StopResult.Stop, result);
    }

    [Fact]
    public void ShouldStop_NonEosToken_ReturnsContinue()
    {
        var condition = new EosStopCondition(eosTokenId: 2);

        var result = condition.ShouldStop(tokenId: 5, generatedTokens: [5], decodedText: "hello");

        Assert.Equal(StopResult.Continue, result);
    }
}
