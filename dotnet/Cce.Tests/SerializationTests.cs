using System;
using System.IO;
using CNET.Cce;
using Xunit;

namespace CNET.Cce.Tests;

public class SerializationTests
{
    [Fact]
    public void EmptyModel_Save_Then_Load_RoundTrips_Metadata()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_model_{Guid.NewGuid():N}.cce");
        try
        {
            using (var model = new CceModel("roundtrip"))
            {
                model.SetDiffMode(CceDiffMode.Hybrid);
                using var sched = new CceScheduler(new CceSchedulerConfig(CceSchedulerType.Warmup, 0.02f));
                model.SetScheduler(sched);
                model.Save(path);
            }

            Assert.True(File.Exists(path));

            using var loaded = CceModel.Load(path);   // must not throw; 0 forests is valid
            Assert.NotNull(loaded);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void Load_Of_Missing_File_Throws()
    {
        Assert.Throws<InvalidOperationException>(() =>
            CceModel.Load(Path.Combine(Path.GetTempPath(), "does_not_exist_cce_bundle.cce")));
    }
}
