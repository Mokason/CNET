# CCE .NET 10 Training API

Modern, safe, high-performance .NET 10 bindings for the CNET / CCE (Compositional Cascade Engine) native library.

## Running Tests

You can now run the full test surface through .NET:

```bash
# From repo root
dotnet test dotnet/Cce.Tests/Cce.Tests.csproj -c Release
```

This uses **xUnit**. The test project automatically attempts to locate/copy `cce.dll` from the repository root.

Example tests cover:
- Dataset creation (FromArrays + WrapArrays)
- Host-side transforms (`OneHot`, row L2 normalization, `FromLabels`)
- Batch iteration and shuffling
- Scheduler behavior
- Model configuration + dispatch paths (`Train`, `TrainBatch`, `InferBatch`)
- Raw routed outputs (`Forward`, `ForwardBatch`) for metrics and inspection
- Model `Save` / `Load`
- `CceHandle` (archive open + adapt/infer)

## Benchmarks

```bash
dotnet run -c Release --project dotnet/Cce.Benchmarks/Cce.Benchmarks.csproj
```

Uses **BenchmarkDotNet**. Current benchmarks include:
- Dataset iteration throughput
- `TrainBatch` and `InferBatch`
- Scheduler LR calculation
- Full (simulated) epoch dispatch cost

You can filter:

```bash
dotnet run -c Release --project ... -- --filter "*TrainBatch*"
```

## Prerequisites

- Build the native library first: `make cce_dll`
- `cce.dll` (or `.so`/`.dylib`) must be discoverable. The projects will attempt to copy it from the repo root during build.

The C core stays in pure C11 with its unique strengths:
- Local learning (NoProp/DFA/Free-Energy style) + optional high-quality hybrid/exact tails
- Verifiable contracts and compositional structure (now accessible from .NET via CcePerceptual, CceRouter, CceForest from packed models)
- Memory-mapped .cce archives + tiered HOT/WARM/COLD
- Per-branch `diff_mode` and schedulers

The .NET layer gives you a pleasant PyTorch-like surface without hiding the philosophy.

## PyTorch-Comparison Status

CCE now covers the practical library surface needed for ordinary host use:

- Dataset construction, batching, shuffling, label one-hot encoding, and row normalization
- Deterministic train/validation split helpers for array and label datasets
- Label smoothing through host-side one-hot target transforms
- Raw routed outputs through `Forward` / `ForwardBatch` for metrics and inspection
- Prediction helpers through `Predict`, `PredictBatch`, and softmax `PredictProbabilities`
- Host-side metrics such as accuracy, top-k, and confusion matrices
- Host-side validation loss for MSE and cross-entropy curves
- Aggregate classifier reports for validation/evaluation loops
- Regression metrics and reports over raw routed outputs
- Native schedulers with stable unmanaged lifetime from .NET
- Managed training presets for local, hybrid, and exact fine-tuning workflows
- Managed training history through `FitHistory` for plotting/logging epoch traces
- Batch and epoch callbacks through `CceCallbacks` for progress logging and early stopping
- Reusable managed early-stopping helper for train/validation monitors
- Managed checkpoint helpers and checkpoint callbacks over native `.cce` model bundles
- Model save/load over archive-backed forests
- Composition-first model definition through forests, branch relations, linear/patch branch factories, and `CceCascadeBuilder`
- Inspectability through `CceForest.Describe()` for branch names, cascade blocks, shapes, parameter counts, and typed composition relations

The intentional difference from PyTorch is architectural: CCE does not expose one global autograd object graph or a monolithic module tree. It keeps the durable unit as a C cascade inside an archive-backed forest branch, with local/hybrid/exact learning modes selected per model, forest, or branch.

## Honest Positioning: Where CCE Reduces PyTorch Pain

CCE is **not** trying to beat PyTorch at large-scale research training or giant models.

**Where CCE aims to feel better (or "PyTorch-like but lighter"):**

- Small/medium local learning loops (no full autograd engine, no Python).
- Many small specialists composed via typed forests + router (instead of one huge `nn.Module`).
- Online / continual / per-sample adaptation (cheap `Adapt` on frozen or live specialists).
- CPU-only or embedded deployment where binary size, startup time, and RAM matter (mmap partial loading of .cce archives, no Python interpreter).
- Applications that cannot or do not want to ship PyTorch / Python.
- Verifiable, contract-based composition (you can certify and freeze small pieces).

**Where PyTorch (or libtorch via TorchSharp) is still the right tool:**

- Very large neural networks
- Heavy GPU training with complex autograd graphs
- Using the massive pretrained model ecosystem
- Rapid research iteration with exotic layers/optimizers

**Where TensorFlow is still orders of magnitude ahead:**

- Breadth and maturity of autograd
- GPU/TPU + distributed + XLA and "Just works" for research-style large differentiable programs
- Production infrastructure at scale

The goal of the .NET surface is to give you a familiar training loop (`Fit`, `DataLoader`-style iteration, schedulers, metrics, device hooks) **for the parts of the world where the monolithic approach is overkill or undesirable**.

If your workload is "train or adapt dozens or hundreds of small, composable, persistable specialists with local credit assignment", CCE + .NET can feel surprisingly productive while staying tiny and embeddable.

## Requirements

- .NET 10 (or .NET 8/9+)
- The native library built as a DLL/shared lib:
  ```bash
  make cce_dll
  ```
- `cce.dll` (Windows) / `cce.so` (Linux) / `cce.dylib` (macOS) must be discoverable (next to your exe, in PATH, or loaded explicitly via `NativeLibrary`).

## Project Setup (csproj)

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <PublishAot>true</PublishAot>          <!-- Excellent with LibraryImport -->
    <IsTrimmable>true</IsTrimmable>
  </PropertyGroup>

  <ItemGroup>
    <!-- Copy the native lib next to your output during build -->
    <None Include="native\cce.dll" CopyToOutputDirectory="PreserveNewest" />
  </ItemGroup>
</Project>
```

## Core Types

| Type              | Purpose |
|-------------------|---------|
| `CceModel`        | High-level training container. Owns forests + scheduler + diff mode |
| `CceDataset`      | Iterator over training data. Supports copy (`FromArrays`) and zero-copy (`WrapArrays` / `WrapMemory`) |
| `CceTransforms`   | Host-side array transforms: one-hot labels and row L2 normalization |
| `CceTrainingBatch`| `ref struct` with `ReadOnlySpan<float>` views. Zero allocation hot path |
| `CceScheduler`    | Cosine / Warmup / Plateau / Step schedulers (native logic) |
| `CceForest`       | SafeHandle wrapper over archive-backed CCE forests and branch composition |
| `CceSpecialistLibrary` | Manager for loading/sealing many forests + memory reporting |
| `CceCascadeBuilder` | Compositional branch builder for custom CCE specialist cascades |
| `CceMetrics`      | Host-side metrics such as `Accuracy` and `TopK` |
| `CceHandle`       | Lower-level archive handle for `Open` + per-sample `Adapt` / `Infer` |
| `CceDiffMode`     | `Local \| Hybrid \| Exact` |
| `CceTrainingConfig` | Epochs, target loss, shuffle, scheduler, diff mode, loss, learner knobs |

## Basic Training Example

```csharp
using CNET.Cce;

float[] inputs = LoadYourInputs();   // perceptual or symbolic feature vectors
int[] labels = LoadYourLabels();

using var dataset = CceDataset.FromLabels(
    inputs,
    labels,
    nSamples: 5000,
    inDim: 35,
    classCount: 10,
    batchSize: 64,
    normalizeRowsL2: true);

using var model = new CceModel("glyph-7seg-v1");

// Optional: attach a scheduler
var scheduler = new CceScheduler(new CceSchedulerConfig(
    Type: CceSchedulerType.Cosine,
    InitialLr: 0.015f,
    WarmupEpochs: 4));

model.SetScheduler(scheduler);
model.SetDiffMode(CceDiffMode.Hybrid);   // exact on last layers, local before

// High-level training (dispatches using router + per-forest/branch diff modes)
double finalLoss = model.Train(dataset, new CceTrainingConfig
{
    MaxEpochs = 60,
    TargetLoss = 0.008f,
    ShuffleEachEpoch = true,
    DiffMode = CceDiffMode.Hybrid,
    Scheduler = new CceSchedulerConfig(CceSchedulerType.Cosine, 0.015f)
});

// Presets are optional recipes over CceTrainingConfig, not hidden native policy.
CceTrainingConfig preset = CceTrainingPresets.HybridQuality(maxEpochs: 40, initialLr: 0.012f);
double presetLoss = model.Train(dataset, preset);

Console.WriteLine($"Final loss: {finalLoss:F5}");

CceTrainingHistory history = model.FitHistory(dataset, new CceTrainingConfig { MaxEpochs = 10 });
foreach (var epoch in history.Epochs)
    Console.WriteLine($"epoch={epoch.Epoch} loss={epoch.TrainLoss:F5} valLoss={epoch.ValLoss:F5} valAcc={epoch.ValAccuracy:P1}");
foreach (var batchInfo in history.Batches)
    Console.WriteLine($"epoch={batchInfo.Epoch} batch={batchInfo.Batch} loss={batchInfo.TrainLoss:F5}");

var earlyStopping = new CceEarlyStopping(patience: 5, monitor: CceEarlyStoppingMonitor.ValidationLoss);
var checkpointEveryEpoch = new CceCheckpointCallback(model, "checkpoints", everyNEpochs: 1);
var logging = new CceCallbacks { OnEpochEnd = e => { Console.WriteLine(e); return true; } };
var callbacks = CceCallbacks.Combine(
    logging,
    new CceCallbacks { OnEpochEnd = checkpointEveryEpoch.OnEpochEnd },
    new CceCallbacks { OnEpochEnd = earlyStopping.OnEpochEnd });
model.Fit(split.Train, preset, callbacks, split.Validation);

model.Save("glyph-7seg-v1.cce");
using var loaded = CceModel.Load("glyph-7seg-v1.cce");

CceCheckpointInfo checkpoint = CceCheckpoint.Save(model, "checkpoints", "epoch-0010");
using var restored = CceCheckpoint.Load(checkpoint.FullPath);
CceCheckpointInfo? latest = CceCheckpoint.Latest("checkpoints");
```

## Custom Training Loop (full control)

```csharp
dataset.Reset();
dataset.Shuffle();

while (dataset.NextBatch(out var batch))
{
    double loss = model.TrainBatch(batch);

    // You can do your own logging, logging to tensorboard, etc.
    if (batch.Index % 500 == 0)
        Console.WriteLine($"Step {batch.Index}: loss={loss:F4}");
}
```

## Zero-Copy from Tile Memory or Perceptual Renders

```csharp
// Suppose you extracted float[] vecs + labels from TileMemory or a perceptual renderer
float[] tileVectors = ...;     // L2-normalized tile embeddings
float[] dummyTargets = ...;    // or one-hot from labels

using var ds = CceDataset.WrapArrays(tileVectors, dummyTargets, n, dim, 1, 128);
// The original tileVectors array must stay alive as long as ds
```

For a true first-class `FromTileMemory`, you can add a helper that walks the TileMemory hot tier, flattens the `vec` fields, and calls `WrapMemory`.

## Host-Side Transforms

```csharp
float[] normalized = CceTransforms.NormalizeRowsL2(rawInputs, rows: n, dim: inDim);
float[] targets = CceTransforms.OneHot(labels, classCount: 10);

using var ds = CceDataset.FromArrays(normalized, targets, n, inDim, 10, batchSize: 64);

// Equivalent convenience path for ordinary classification tables:
using var cls = CceDataset.FromLabels(
    rawInputs,
    labels,
    n,
    inDim,
    classCount: 10,
    batchSize: 64,
    normalizeRowsL2: true,
    labelSmoothing: 0.05f);

using var split = CceDataset.SplitFromLabels(
    rawInputs,
    labels,
    n,
    inDim,
    classCount: 10,
    batchSize: 64,
    validationFraction: 0.2f,
    normalizeRowsL2: true,
    labelSmoothing: 0.05f,
    seed: 123);

model.FitHistory(split.Train, validation: split.Validation);
```

## Online Adaptation (the classic CCE adapt path)

```csharp
using var handle = CceHandle.Open("artifacts/glyph_habitat/perceptual_subforests.cce");

handle.SetDiffMode(CceDiffMode.Local);

for (int i = 0; i < 10000; i++)
{
    var sample = GetNextInputVector();
    int label = GetLabel(sample);

    handle.Adapt(sample, label, lr: 0.01f);
}
```

Attach a scheduler the same way as on `CceModel`.

## Inference

```csharp
var (label, confidence) = model.Infer(myInputVector);

// or batched
int[] labels = new int[batch.BatchSize];
float[] confs = new float[batch.BatchSize];
model.InferBatch(batch, labels, confs);

// raw routed cascade outputs before label reduction
float[] logits = model.Forward(myInputVector, outputDim: 10);
float[] batchLogits = model.ForwardBatch(batch, outputDim: 10);
int predicted = model.Predict(myInputVector, outputDim: 10);
float[] probabilities = model.PredictProbabilities(myInputVector, outputDim: 10);
int[] batchPredictions = model.PredictBatch(batch, outputDim: 10);
float[] batchProbabilities = model.PredictProbabilitiesBatch(batch, outputDim: 10);

float acc = CceMetrics.Accuracy(model, validationSet);
double valLoss = CceMetrics.Loss(model, validationSet, CceLossType.CrossEntropy);
float top3 = CceMetrics.TopK(model, validationSet, k: 3);
int[,] confusion = CceMetrics.ConfusionMatrix(model, validationSet);
CceClassificationReport report = CceMetrics.ClassificationReport(model, validationSet, topK: 3);
CceRegressionReport regression = CceMetrics.RegressionReport(model, validationSet);
```

## Optional Autograd (Exact Tails Only)

Autograd is a small reverse-mode tape that lives **inside** CCE. It is used **only** when you select `CceDiffMode.Exact` **and** `UseAutogradExactTail = true`.

```csharp
var cfg = new CceTrainingConfig
{
    DiffMode = CceDiffMode.Exact,
    UseAutogradExactTail = true,
    MaxEpochs = 20
};

using var agModel = new CceModel("exact-head");
agModel.SetDevice(CceDevice.Cpu); // or Cuda later
double loss = agModel.Fit(dataset, cfg);
```

Local and hybrid modes are completely unaffected and never allocate an autograd tape.

See "Autograd is opt-in" philosophy in the main project positioning.

## GPU Acceleration

CCE can use CUDA (when the native is built with it) for parts of training (matmul + Adam updates in the learner for blocks that benefit).

```csharp
using var model = new CceModel("my-specialists");

bool usingGpu = model.TryUseGpu();                 // preferred
// or
model.SetDevice(CceDevice.Cuda);                   // or Auto
model.UseDevice(CceDevice.Cuda);                   // extension

var config = new CceTrainingConfig { MaxEpochs = 30, ... };
double loss = model.Fit(dataset, config);
```

To get a CUDA-enabled `cce.dll`:
- Build with the equivalent of `CCE_USE_CUDA=1` + proper nvcc / CUDA toolkit in your environment (see top-level Makefile and `src/cce/cce_gpu.c`).
- The .NET API stays the same; if CUDA is not present it gracefully stays on CPU.

This is especially attractive for **CPU-fallback + occasional GPU boost** scenarios or embedded devices that occasionally have a GPU.

## Loading a Whole Library of Specialists + Memory Reporting

For the "many small specialists" scenario:

```csharp
using var lib = new CceSpecialistLibrary();
lib.LoadDirectory("models/specialists/", sealImmediately: true);
lib.SealAll();

var head = lib["digit_classifier"];
Console.WriteLine(lib.GetMemoryReport());   // approximate param bytes + advice

// Fast zero-copy inference after sealing
var logits = head.ForwardBranch(0, myFeatures, outDim: 10);
```

This gives much lower startup + RSS than loading equivalent number of PyTorch modules.

## Sealed Zero-Copy Fast Inference + Latency

```csharp
using var forest = CceForest.Open("my_specialists.cce");
forest.Seal();                    // critical for zero-copy WARM views

var logits = forest.ForwardBranch(branchIdx, input, 10);
int label = model.Predict(input, 10);   // after model.SealForInference()
```

After `Seal()`, `cce_forest_forward` / model inference reuses mmap pages with no extra allocation or reload.

See `CceInferenceLatencyBenchmarks` in the Benchmarks project for measuring single-sample and branch-forward latency.

## Importing Pre-trained Tiny Heads (Weight Import Helpers)

Train a small linear (or head) elsewhere, export the weights+bias as float[], then:

```csharp
int branch = forest.AddLinearBranch("imported_head", inDim: 32, hiddenDim: 16, outputDim: 5);
forest.SetLinearBranchWeights(branch, pretrainedWeights, inDim: 32, outDim: 5, pretrainedBias);
```

Enables using external small models as CCE specialists without full retraining inside CCE.

## Aggressive CPU Inference, Tiling, Fastpath & AOT

- The core tensor ops use tiled GEMM.
- Use `Seal()` + direct `ForwardBranch` or `Predict` for minimal overhead.
- The package is already configured for `PublishAot` and `IsAotCompatible`.
- Recommended: `<PublishTrimmed>true</PublishTrimmed>` + NativeAOT for embedded.
- For hottest paths, `Predict` / `Infer` use spans; keep input data pinned when possible.
- Low-level `CceHandle` + per-sample `Adapt` / `Infer` has the absolute smallest overhead for embedded online use.

See root project `make fastpath` / `throughput` for C-side fast execution lanes (also available through the ABI).

## Full High-Level CNET Stack from .NET (Router / Planner / Contracts / Perceptual / Glyph Habitat)

Recent updates bridge the low-level CCE (including 1.6-bit packed models) to the full compositional layer:

- `CceSupraA2A.LoadPacked(...)` + `GetSpecialistsForest()` — get the w_trit specialists as a regular `CceForest`.
- `CcePerceptual` — create/train perceptual leaves (glyph/7seg etc.).
- `CceRouter` — route over forests.
- `CceContract` — basic contract authoring/registration for specialists.
- Use the forests directly with `CceModel.Add(forest, "name")`, `CceSpecialistLibrary`, or compose them.

This lets you drive the same patterns as `tests/glyph_habitat.c` (perceptual + contracts + planner + narrative) from C#.

Example:
```csharp
using var supra = CceSupraA2A.LoadPacked("my_1p6bit.bin");
var specialists = supra.GetSpecialistsForest();

using var percept = CcePerceptual.Create(35, 10, "glyph");
percept.Train();

using var model = new CceModel("composed");
model.Add(specialists, "supra-1p6");
model.Add(percept.GetForest(), "percept");

var router = new CceRouter();
var contract = CceContract.Create("my_task");
// register & compose...
```

**Addressed limitations (this update):**
- Lifetime: `GetSpecialistsForest()` now attaches the Supra as owner — no manual "keep alive".
- High-level: Incremental but usable C# surface for the glyph_habitat patterns (more marshaling for full dag_plan/PrimitiveRegistry can be added on demand).
- Packed: Inference works everywhere (forwards, router, perceptual). For training a packed/ternary model, train the non-packed version (or use supra_head_qat style) then export packed.
- Build: After C edits: `make cce_dll` (or mingw32-make / your cross compiler) then copy cce.dll next to your .NET app.

## Adding Forests

Forests are typically created by loading `.cce` archives on the C side, via perceptual leaf construction, or by opening an archive-backed forest handle.

```csharp
using var forest = CceForest.Open("my_specialists.cce", maxBranches: 64);
forest.SetDiffMode(CceDiffMode.Hybrid);
int branch = forest.AddLinearBranch("digits", inputDim: 35, hiddenDim: 64, outputDim: 10);
int repair = forest.AddLinearBranch("digits_repair", inputDim: 35, hiddenDim: 64, outputDim: 10);
int patch = forest.AddPatchBranch("digit_patches", patchSize: 2, stride: 1, channels: 1, hiddenDim: 32, outputDim: 10);
forest.Connect(branch, repair, CceConnectionType.Refines);
forest.Connect(patch, branch, CceConnectionType.Composes);
forest.SetBranchDiffMode(repair, CceDiffMode.Exact);
forest.SetBranchExactTailLength(repair, 1);
model.Add(forest, "symbolic-retrieval");

foreach (var edge in forest.GetConnections(branch))
    Console.WriteLine($"{forest.BranchName(branch)} --{edge.Type}--> {edge.Name}");

Console.WriteLine(forest.Describe());
```

For custom specialists, build one cascade from CCE primitives and move it into the forest:

```csharp
using var custom = CceCascadeBuilder.Create(maxBlocks: 3)
    .AddPatch(patchSize: 2, stride: 1, channels: 1)
    .AddLinear(inputDim: 4, outputDim: 32)
    .AddLinearHead(inputDim: 32, outputDim: 10);

int customBranch = custom.AddTo(forest, "custom-patch-specialist");
```

## Design Notes for .NET 10

- Uses `[LibraryImport]` + source generation (no runtime marshalling overhead, great for NativeAOT).
- `CceTrainingBatch` is a `ref struct` → zero heap allocation on the hot training path.
- Pinning is explicit and lifetime-controlled for `Wrap*` paths.
- `CceModel.Train(...)` internally uses the native router + per-branch `diff_mode` dispatch you built in the C layer.
- Schedulers are small C structs stored in stable unmanaged memory by the .NET wrapper, then attached by pointer to models/handles.

## Full Example: Perceptual + Symbolic

See `Examples/PerceptualTraining.cs` (or the code below).

```csharp
// 1. Train perceptual specialists (7-seg / glyph / grid) with EXACT on ambiguity heads
// 2. Train a symbolic / retrieval head on tile embeddings using HYBRID
// 3. Compose them inside one CceModel
```

The native dispatch (names + router) will route the right inputs to the right sub-forests.

## DataLoader and Materialized Batches

CCE provides both a high-performance ref-struct zero-copy path and a safe materialized path:

```csharp
using var ds = CceDataset.FromGaussianBlobs(2048, 16, 5, batchSize: 64);

// Safe, storable batches (recommended for most code, replay buffers, etc.)
using var loader = ds.AsDataLoader(materialize: true);
foreach (var mb in loader)
{
    // mb.Inputs is a float[] you can keep
    double loss = model.TrainBatch(mb.AsView());
}

// Hot zero-copy (use the callback form or iterate the dataset directly)
loader.StreamRefBatches(view =>
{
    double loss = model.TrainBatch(view);
}, shuffleFirst: true);

// Or for full control in hot loops:
dataset.Reset();
while (dataset.NextBatch(out var view))
{
    /* ... */
}
```

`CceMaterializedBatch` is a value type with owned arrays. `CceTrainingBatch` remains the ephemeral high-speed view.

## Synthetic Datasets for Quick Experiments

```csharp
// Classification with separable blobs
using var blobs = CceDataset.FromGaussianBlobs(3000, inDim: 12, classCount: 4, batchSize: 32);

// Non-linear toy problem
using var xor = CceDataset.FromXorParity(2000, inDim: 4, batchSize: 64);

// Simple regression
using var reg = CceDataset.ForLinearRegression(1500, inDim: 8, outDim: 1, batchSize: 32, noiseStd: 0.05);
```

These are invaluable for the benchmark suite and for testing new specialists before wiring real perceptual/tile data.

## Benchmarks and PyTorch-style Baselines

```bash
dotnet run -c Release --project dotnet/Cce.Benchmarks/Cce.Benchmarks.csproj
```

The benchmark runner now includes:
- Micro-benches (dataset throughput, TrainBatch, scheduler)
- An end-to-end training report (Gaussian blobs) that prints loss, accuracy, epochs/sec, and explicit guidance on how to build a comparable PyTorch / TorchSharp baseline.

The goal of CCE is **not** to win single-model throughput benchmarks. It is to make compositional, contract-verified, archive-persistable specialists first-class so that many small frozen nets can be routed and reused without monolithic retraining. Use the report numbers to show where the trade-offs are.

## Common Pitfalls & Lifetime Rules

- **Do not dispose forests added to a model.** The model keeps them alive. Disposing the forest while the model is live leads to use-after-free.
- **WrapArrays / WrapMemory callers must keep source arrays alive** for the full lifetime of the dataset + any batches derived from it.
- `CceTrainingBatch` is a `ref struct` — you cannot store it in a List<>, return it from most methods, or capture it in lambdas that outlive the loop step.
- Use `Save`/`Load` on `CceModel` for self-contained bundles (includes forests + scheduler state).
- `DiffMode.Exact` on deep cascades is higher quality but loses some of the pure local-learning composition guarantees.
- Rebuild `cce.dll` (`make cce_dll`) after any change to the C sources before running .NET tests/apps.

## Future Enhancements (easy to add)

- Direct `FromTileMemory(TileMemory tm, ...)` that extracts vecs
- `CcePerceptual` helpers that render 7-seg/glyph on the fly into a dataset
- Better `ReadOnlySpan<float>` overloads without copies for single infer
- Structured logging hooks / `IProgress<TrainingProgress>`
- GPU acceleration for training (call `model.TryUseGpu()` or `model.SetDevice(CceDevice.Cuda)`) when the native `cce.dll` was built with CUDA support. Small-to-medium cascades get real speedups on the local/hybrid paths without pulling in a full DL framework. See "GPU" section below.

## License / Philosophy

Same as the main CNET project: keep the core in verifiable C while giving first-class modern hosts (here: .NET 10) a clean, fast training surface.

Enjoy composing!
