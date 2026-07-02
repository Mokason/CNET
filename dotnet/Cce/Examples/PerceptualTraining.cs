// Example: Using the .NET 10 CCE API for perceptual + symbolic training.
// Assumes you have already built cce.dll via `make cce_dll`.

using System;
using CNET.Cce;

namespace CNET.Cce.Examples;

public static class PerceptualTraining
{
    public static void Run()
    {
        Console.WriteLine("=== CCE .NET 10 Perceptual + Symbolic Training Example ===");

        // ------------------------------------------------------------
        // 1. Create synthetic training data
        //    In real life this would come from:
        //    - cce_perceptual_leaf rendering (7-seg, glyph, grid, block)
        //    - TileMemory vecs (symbolic / retrieval memory)
        // ------------------------------------------------------------

        const int nSamples = 2048;
        const int inDim = 35;     // 5x7 7-segment style or flattened perceptual
        const int outDim = 10;    // digit classes
        const int batchSize = 64;

        float[] inputs = new float[nSamples * inDim];
        float[] targets = new float[nSamples * outDim];

        var rnd = new Random(123);
        for (int i = 0; i < nSamples; i++)
        {
            // Fake "rendered" input (in real code: perceptual render or tile vec)
            for (int d = 0; d < inDim; d++)
                inputs[i * inDim + d] = (float)(rnd.NextDouble() * 2 - 1);

            // One-hot target
            int label = i % 10;
            targets[i * outDim + label] = 1.0f;
        }

        // ------------------------------------------------------------
        // 2. Dataset (zero-copy capable)
        // ------------------------------------------------------------

        using var dataset = CceDataset.FromArrays(inputs, targets, nSamples, inDim, outDim, batchSize);
        // Or for external memory (tile memory, large perceptual buffers):
        // using var dataset = CceDataset.WrapArrays(managedInputs, managedTargets, ...);

        // ------------------------------------------------------------
        // 3. Model + modern configuration
        // ------------------------------------------------------------

        using var model = new CceModel("percept-symbolic-v1");

        // Use HYBRID: exact gradients on the tail (good for output heads), local credit before
        model.SetDiffMode(CceDiffMode.Hybrid);

        var scheduler = new CceScheduler(new CceSchedulerConfig(
            Type: CceSchedulerType.Cosine,
            InitialLr: 0.012f,
            WarmupEpochs: 5));

        model.SetScheduler(scheduler);

        // If you had multiple forests loaded from .cce archives you could do:
        // IntPtr perceptualForest = ...;   // from C-side archive load or perceptual construction
        // model.AddForest(perceptualForest, "perceptual-7seg");
        //
        // IntPtr symbolicForest = ...;
        // model.AddForest(symbolicForest, "tile-retrieval");

        // NEW: Use 1.6-bit packed Supra model specialists + perceptual/router for full composition:
        // using var supra = CceSupraA2A.LoadPacked("artifacts/supra_packed_1p6.bin");
        // var supraForest = supra.GetSpecialistsForest();   // w_trit packed branches, usable everywhere
        // model.Add(supraForest, "supra-1p6-specialists");
        //
        // using var percept = CcePerceptual.Create(35, 10, "glyph");
        // percept.Train();
        // var perceptForest = percept.GetForest();
        // model.Add(perceptForest, "percept-glyph");
        //
        // CceRouter router = new CceRouter(temperature: 0.8f, topK: 4);
        // Then use router.Route(...) or let CceModel training dispatch handle routing + contracts.

        // Deeper: use CceRegistry + CceContract for PrimitiveRegistry/planner with CCE forests (packed OK)
        // using var reg = new CceRegistry();
        // using var c = CceContract.Create("my-contract");
        // reg.AddCceSpecialist(supraForest, "supra-1p6");
        // reg.AddContractedSpecialist(c, "contracted-task");
        // (enables dag_plan / route_plan style composition from C#)

        // The native train dispatch will route using router + names.

        // ------------------------------------------------------------
        // 4. Train (high-level)
        // ------------------------------------------------------------

        var config = new CceTrainingConfig
        {
            MaxEpochs = 25,
            TargetLoss = 0.02f,
            ShuffleEachEpoch = true,
            DiffMode = CceDiffMode.Hybrid,
            Scheduler = new CceSchedulerConfig(CceSchedulerType.Cosine, 0.012f)
        };

        Console.WriteLine("Starting training...");
        double loss = model.Train(dataset, config);
        Console.WriteLine($"Training complete. Final loss ≈ {loss:F5}");

        // ------------------------------------------------------------
        // 5. Inference
        // ------------------------------------------------------------

        dataset.Reset();
        if (dataset.NextBatch(out var batch))
        {
            int[] predicted = new int[batch.BatchSize];
            float[] conf = new float[batch.BatchSize];

            model.InferBatch(batch, predicted, conf);

            Console.WriteLine($"Sample batch inference:");
            for (int i = 0; i < Math.Min(5, batch.BatchSize); i++)
                Console.WriteLine($"  idx {batch.Index + i}: pred={predicted[i]}, conf={conf[i]:F3}");
        }

        // Single sample
        var (label, confidence) = model.Infer(new ReadOnlySpan<float>(inputs, 0, inDim));
        Console.WriteLine($"Single infer: label={label}, conf={confidence:F3}");

        // ------------------------------------------------------------
        // 6. (Optional) Low-level online adaptation path
        // ------------------------------------------------------------

        // using var handle = CceHandle.Open("artifacts/glyph_habitat/perceptual_subforests.cce");
        // handle.SetDiffMode(CceDiffMode.Local);
        // handle.Adapt(someVector, label: 7, lr: 0.007f);

        Console.WriteLine("Done. The C core did the real work (dispatch, diff modes, scheduler, local credit).");

        // ------------------------------------------------------------
        // 7. DataLoader + synthetic data demo (new)
        // ------------------------------------------------------------
        Console.WriteLine("\n--- DataLoader + synthetic demo ---");
        using var synth = CceDataset.FromGaussianBlobs(512, inDim: 8, classCount: 3, batchSize: 32, seed: 99);
        using var loader = synth.AsDataLoader(materialize: true);

        int batchCount = 0;
        foreach (var mb in loader)
        {
            batchCount++;
            if (batchCount == 1)
                Console.WriteLine($"First materialized batch: {mb}");
        }
        Console.WriteLine($"Iterated {batchCount} materialized batches via CceDataLoader.");

        // ------------------------------------------------------------
        // 8. Autograd exact tail demo: frozen CCE + UseAutogradExactTail head
        // ------------------------------------------------------------
        Console.WriteLine("\n--- Autograd exact tail (UseAutogradExactTail = true) ---");
        // The CceModel with the flag will use the ag tape for the exact tail in its training path.
        // Canonical: CCE cascade/forest as "feature" part, ag for the head.
        using var modelWithHead = new CceModel("cce-plus-ag-head");
        modelWithHead.SetDiffMode(CceDiffMode.Exact);

        var configWithAg = new CceTrainingConfig {
            MaxEpochs = 3,
            DiffMode = CceDiffMode.Exact,
            UseAutogradExactTail = true,   // <--- the key flag
            Loss = CceLossType.CrossEntropy
        };

        // synthetic "data" (in real: features from a frozen CCE forest + labels)
        float[] synIn = new float[16 * 4];
        int[] synLab = new int[16];
        var rr = new Random(99);
        for (int i = 0; i < 16; i++) {
            for (int d = 0; d < 4; d++) synIn[i*4+d] = (float)(rr.NextDouble()*2-1);
            synLab[i] = i % 3;
        }
        using var synDs = CceDataset.FromLabels(synIn, synLab, 16, 4, 3, 4);

        Console.WriteLine("Training CceModel with UseAutogradExactTail=true (ag tape for tail/head)...");
        double finalL = modelWithHead.Train(synDs, configWithAg);
        Console.WriteLine($"Final loss with ag exact tail: {finalL:F4}");
        Console.WriteLine("The autograd tape was used only for the exact head in the training path.");
    }
}
