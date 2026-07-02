using System;
using System.IO;
using CNET.Cce;
using Xunit;

namespace CNET.Cce.Tests;

public class CheckpointTests
{
    [Fact]
    public void Checkpoint_Save_Load_And_Latest_Use_Model_Persistence()
    {
        string root = Path.Combine(Path.GetTempPath(), $"cce_ckpt_{Guid.NewGuid():N}");
        string forestPath = Path.Combine(root, "forest.cce");
        string checkpointDir = Path.Combine(root, "checkpoints");
        try
        {
            Directory.CreateDirectory(root);
            using var forest = CceForest.Open(forestPath, maxBranches: 4);
            forest.AddLinearBranch("checkpointed", inputDim: 4, hiddenDim: 6, outputDim: 2);

            using var model = new CceModel("checkpoint-model");
            model.Add(forest, "f");

            float[] input = { 1f, 0.25f, -0.5f, 0.75f };
            float[] reference = model.Forward(input, outputDim: 2);

            CceCheckpointInfo saved = CceCheckpoint.Save(model, checkpointDir, "epoch-0001");
            CceCheckpointInfo? latest = CceCheckpoint.Latest(checkpointDir);

            Assert.True(File.Exists(saved.FullPath));
            Assert.Equal(saved.FullPath, latest?.FullPath);

            using var loaded = CceCheckpoint.Load(saved.FullPath);
            float[] actual = loaded.Forward(input, outputDim: 2);

            Assert.Equal(reference.Length, actual.Length);
            Assert.Equal(reference[0], actual[0], precision: 5);
            Assert.Equal(reference[1], actual[1], precision: 5);
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void Checkpoint_Save_Sanitizes_Path_Separators_In_Name()
    {
        string root = Path.Combine(Path.GetTempPath(), $"cce_ckpt_safe_{Guid.NewGuid():N}");
        string forestPath = Path.Combine(root, "forest.cce");
        string checkpointDir = Path.Combine(root, "checkpoints");
        try
        {
            Directory.CreateDirectory(root);
            using var forest = CceForest.Open(forestPath, maxBranches: 4);
            forest.AddLinearBranch("checkpointed", inputDim: 4, hiddenDim: 6, outputDim: 2);

            using var model = new CceModel("checkpoint-safe-name");
            model.Add(forest, "f");

            CceCheckpointInfo saved = CceCheckpoint.Save(model, checkpointDir, "../unsafe/name");
            string savedFileName = Path.GetFileName(saved.FullPath);

            Assert.Equal(checkpointDir, Path.GetDirectoryName(saved.FullPath));
            Assert.False(savedFileName.Contains("..", StringComparison.Ordinal));
            Assert.False(savedFileName.Contains("/", StringComparison.Ordinal));
            Assert.False(savedFileName.Contains("\\", StringComparison.Ordinal));
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void CheckpointCallback_Saves_On_Configured_Epochs()
    {
        string root = Path.Combine(Path.GetTempPath(), $"cce_ckpt_cb_{Guid.NewGuid():N}");
        string forestPath = Path.Combine(root, "forest.cce");
        string checkpointDir = Path.Combine(root, "checkpoints");
        try
        {
            Directory.CreateDirectory(root);
            using var forest = CceForest.Open(forestPath, maxBranches: 4);
            forest.AddLinearBranch("checkpointed", inputDim: 4, hiddenDim: 6, outputDim: 2);

            using var model = new CceModel("checkpoint-callback-model");
            model.Add(forest, "f");

            var callback = new CceCheckpointCallback(model, checkpointDir, everyNEpochs: 2);

            Assert.True(callback.OnEpochEnd(new EpochInfo(0, 1.0, null, 0f)));
            Assert.Null(callback.LastCheckpoint);

            Assert.True(callback.OnEpochEnd(new EpochInfo(1, 0.9, null, 0f)));
            Assert.NotNull(callback.LastCheckpoint);
            Assert.True(File.Exists(callback.LastCheckpoint!.FullPath));
            Assert.Equal(callback.LastCheckpoint.FullPath, CceCheckpoint.Latest(checkpointDir)?.FullPath);
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }
}
