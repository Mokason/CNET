using System;
using System.IO;
using CNET.Cce;
using Xunit;

namespace CNET.Cce.Tests;

public class ForestApiTests
{
    [Fact]
    public void Forest_Open_Creates_Empty_Forest_With_Zero_Branches()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_forest_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 8);
            Assert.False(forest.IsInvalid);
            Assert.Equal(0, forest.BranchCount);

            using var model = new CceModel("compose-test");
            model.Add(forest, "subforest_a");
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void Forest_SetDiffMode_Does_Not_Throw()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_forest_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path);
            forest.SetDiffMode(CceDiffMode.Hybrid);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void Forest_AddLinearBranch_Creates_Usable_Specialist()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_forest_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 8);

            int idx = forest.AddLinearBranch("digits", inputDim: 4, hiddenDim: 6, outputDim: 2, initScale: 0.01f);

            Assert.Equal(0, idx);
            Assert.Equal(1, forest.BranchCount);
            Assert.Equal("digits", forest.BranchName(0));

            using var model = new CceModel("builder-test");
            model.Add(forest, "digit-specialists");

            float[] raw = model.Forward(new float[] { 1, 0, 0, 0 }, outputDim: 2);

            Assert.Equal(2, raw.Length);
            Assert.True(float.IsFinite(raw[0]));
            Assert.True(float.IsFinite(raw[1]));
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void Forest_AddPatchBranch_Creates_Usable_Specialist()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_forest_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 8);

            int idx = forest.AddPatchBranch("patches", patchSize: 2, stride: 1, channels: 1, hiddenDim: 5, outputDim: 2);

            Assert.Equal(0, idx);
            Assert.Equal("patches", forest.BranchName(0));

            using var model = new CceModel("patch-builder");
            model.Add(forest, "patch-specialists");

            float[] raw = model.Forward(new float[] { 1f, 0.25f, -0.5f, 0.75f }, outputDim: 2);

            Assert.Equal(2, raw.Length);
            Assert.True(float.IsFinite(raw[0]));
            Assert.True(float.IsFinite(raw[1]));
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void CascadeBuilder_AddToForest_Creates_Usable_Composed_Specialist()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_forest_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 8);
            using var builder = CceCascadeBuilder.Create(maxBlocks: 3)
                .AddPatch(patchSize: 2, stride: 1, channels: 1)
                .AddLinear(inputDim: 4, outputDim: 5)
                .AddLinearHead(inputDim: 5, outputDim: 2);

            int idx = builder.AddTo(forest, "custom-patch-specialist");

            Assert.Equal(0, idx);
            Assert.Equal(1, forest.BranchCount);
            Assert.Equal("custom-patch-specialist", forest.BranchName(0));

            using var model = new CceModel("custom-builder");
            model.Add(forest, "custom-specialists");

            float[] raw = model.Forward(new float[] { 0.2f, -0.1f, 0.7f, 0.4f }, outputDim: 2);

            Assert.Equal(2, raw.Length);
            Assert.True(float.IsFinite(raw[0]));
            Assert.True(float.IsFinite(raw[1]));
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void Forest_Connections_And_Branch_Overrides_Are_Managed()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_forest_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 8);
            int baseBranch = forest.AddLinearBranch("base", inputDim: 4, hiddenDim: 6, outputDim: 2);
            int refinement = forest.AddLinearBranch("refinement", inputDim: 4, hiddenDim: 6, outputDim: 2);

            forest.Connect(baseBranch, refinement, CceConnectionType.Refines);
            forest.SetBranchDiffMode(refinement, CceDiffMode.Exact);
            forest.SetBranchExactTailLength(refinement, 1);

            var connections = forest.GetConnections(baseBranch);
            var summary = forest.Describe();

            Assert.Single(connections);
            Assert.Equal("refinement", connections[0].Name);
            Assert.Equal(CceConnectionType.Refines, connections[0].Type);
            Assert.Equal(2, summary.BranchCount);
            Assert.Equal("base", summary.Branches[baseBranch].Name);
            Assert.Equal(2, summary.Branches[baseBranch].Blocks.Count);
            Assert.Equal(CceBlockKind.Linear, summary.Branches[baseBranch].Blocks[0].Kind);
            Assert.Equal(4, summary.Branches[baseBranch].Blocks[0].InputDim);
            Assert.Equal(6, summary.Branches[baseBranch].Blocks[0].OutputDim);
            Assert.Equal(30L, summary.Branches[baseBranch].Blocks[0].ParameterCount);
            Assert.Equal(CceBlockKind.LinearHead, summary.Branches[baseBranch].Blocks[1].Kind);
            Assert.Equal(14L, summary.Branches[baseBranch].Blocks[1].ParameterCount);
            Assert.Single(summary.Branches[baseBranch].Connections);
            Assert.Contains("base", summary.ToString(), StringComparison.Ordinal);
            Assert.Contains("LinearHead", summary.ToString(), StringComparison.Ordinal);
            Assert.Contains("params=44", summary.ToString(), StringComparison.Ordinal);
            Assert.Contains("Refines", summary.ToString(), StringComparison.Ordinal);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }
}
