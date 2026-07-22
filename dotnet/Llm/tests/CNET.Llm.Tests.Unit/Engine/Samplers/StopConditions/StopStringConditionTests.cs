using CNET.Llm.Core.Sampling;
using CNET.Llm.Engine.Samplers.StopConditions;
using Xunit;

namespace CNET.Llm.Tests.Unit.Engine.Samplers.StopConditions;

public class StopStringConditionTests
{
    [Fact]
    public void ShouldStop_Match_ReturnsStop()
    {
        var condition = new StopStringCondition("END");

        var result = condition.ShouldStop(tokenId: 1, generatedTokens: [1], decodedText: "some text END");

        Assert.Equal(StopResult.Stop, result);
    }

    [Fact]
    public void ShouldStop_NoMatch_ReturnsContinue()
    {
        var condition = new StopStringCondition("END");

        var result = condition.ShouldStop(tokenId: 1, generatedTokens: [1], decodedText: "some text");

        Assert.Equal(StopResult.Continue, result);
    }

    [Fact]
    public void ShouldStop_PartialMatch_ReturnsContinue()
    {
        var condition = new StopStringCondition("END");

        var result = condition.ShouldStop(tokenId: 1, generatedTokens: [1], decodedText: "some text EN");

        Assert.Equal(StopResult.Continue, result);
    }

    [Fact]
    public void ShouldStop_CaseSensitive()
    {
        var condition = new StopStringCondition("END");

        var result = condition.ShouldStop(tokenId: 1, generatedTokens: [1], decodedText: "some text end");

        Assert.Equal(StopResult.Continue, result);
    }
}
