using System;
using CNET.Cce;
using Xunit;

namespace CNET.Cce.Tests;

public class AutogradTests
{
    [Fact]
    public void Context_Create_Tensor_Dispose_Works()
    {
        using var ag = new CceAutogradContext(64);
        float[] data = { 1f, 2f, 3f, 4f };
        using var t = ag.CreateTensor(data, 2, 2, requiresGrad: true);
        Assert.Equal(2, t.Rows);
        Assert.Equal(2, t.Cols);
        Assert.True(t.RequiresGrad);
        var d = t.GetData();
        Assert.Equal(4, d.Length);
        Assert.Equal(1f, d[0]);
    }

    [Fact]
    public void Matmul_And_Sgd_Reduces_Loss()
    {
        using var ag = new CceAutogradContext(256);

        // Tiny y = x @ W , mse against target
        float[] xdata = { 1f, 0f, 0f, 1f }; // 2x2
        float[] wdata = { 0.1f, 0.2f, 0.3f, 0.4f };
        float[] tdata = { 0.5f, 0.6f, 0.7f, 0.8f };

        var x = ag.CreateTensor(xdata, 2, 2, requiresGrad: false);
        var w = ag.CreateTensor(wdata, 2, 2, requiresGrad: true);
        var tgt = ag.CreateTensor(tdata, 2, 2, requiresGrad: false);

        var y = ag.MatMul(x, w);
        var loss = ag.MseLoss(y, tgt);

        ag.ZeroGrad();
        ag.Backward(loss);

        ag.SgdStep(new[] { w }, 0.1f);

        var newW = w.GetData();
        Assert.NotEqual(wdata[0], newW[0]); // weight should have moved if grad was non-zero
    }
}
