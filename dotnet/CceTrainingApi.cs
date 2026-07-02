using System;
using System.Runtime.InteropServices;

namespace CNET.Cce
{
    // LEGACY / minimal sketch kept for reference.
    // 
    // >>> The recommended modern .NET 10 API lives in dotnet/Cce/ <<<
    //     - CceNative.cs   (LibraryImport)
    //     - CceModel.cs, CceDataset.cs, CceScheduler.cs, CceHandle.cs, CceTrainingBatch.cs
    //     - Examples/PerceptualTraining.cs
    //     - README.md
    //
    // The new surface uses:
    //   - [LibraryImport] + source generation (AOT friendly)
    //   - ReadOnlySpan<float> + ref struct batches (zero allocation)
    //   - Proper pinning for wrap paths
    //   - CceTrainingConfig + high-level Train() that respects router dispatch + per-branch diff_mode
    //
    // Build the native library with:  make cce_dll
    //
    // Example:
    //   using var ds = CceDataset.FromArrays(...);
    //   using var model = new CceModel("foo");
    //   model.Train(ds, new CceTrainingConfig { MaxEpochs = 30, DiffMode = CceDiffMode.Hybrid });


    public enum CceDiffMode { Local = 0, Hybrid = 1, Exact = 2 }

    public enum CceSchedType { Cosine = 0, Warmup = 1, Plateau = 2, Step = 3 }

    [StructLayout(LayoutKind.Sequential)]
    public struct CceBatch
    {
        public IntPtr Inputs;
        public IntPtr Targets;
        public UIntPtr BatchSize;
        public UIntPtr InDim;
        public UIntPtr OutDim;
        public UIntPtr Index;
    }

    public static class CceNative
    {
        private const string DllName = "cce"; // or "libcce" depending on build

        // Model
        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cce_model_create(out IntPtr model, string name);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cce_model_destroy(IntPtr model);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cce_model_add_forest(IntPtr model, IntPtr forest, string name);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cce_model_set_scheduler(IntPtr model, IntPtr scheduler);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cce_model_set_diff_mode(IntPtr model, int mode);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern double cce_model_train(IntPtr model, IntPtr dataset, UIntPtr maxEpochs, float targetLoss);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cce_model_infer_batch(IntPtr model, ref CceBatch batch, int[] outLabels, float[] confs);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern double cce_model_train_batch(IntPtr model, ref CceBatch batch);

        // Dataset
        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cce_dataset_from_arrays(out IntPtr ds,
            float[] inputs, float[] targets,
            UIntPtr nSamples, UIntPtr inDim, UIntPtr outDim, UIntPtr batchSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cce_dataset_wrap_arrays(out IntPtr ds,
            float[] inputs, float[] targets,
            UIntPtr nSamples, UIntPtr inDim, UIntPtr outDim, UIntPtr batchSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cce_dataset_next_batch(IntPtr ds, ref CceBatch batch);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cce_dataset_reset(IntPtr ds);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cce_dataset_destroy(IntPtr ds);

        // Scheduler (if you want to create from .NET)
        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cce_scheduler_init(IntPtr sched, int type, float initialLr);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern float cce_scheduler_get_lr(IntPtr sched, int epoch, float currentLoss);
    }

    // High-level .NET Training API
    public class CceModel : IDisposable
    {
        private IntPtr _handle;

        public CceModel(string name)
        {
            if (CceNative.cce_model_create(out _handle, name) != 0)
                throw new Exception("Failed to create CceModel");
        }

        public void AddForest(IntPtr forest, string name)
        {
            if (CceNative.cce_model_add_forest(_handle, forest, name) != 0)
                throw new Exception("Failed to add forest");
        }

        public void SetDiffMode(CceDiffMode mode)
        {
            CceNative.cce_model_set_diff_mode(_handle, (int)mode);
        }

        public double Train(IntPtr dataset, int maxEpochs, float targetLoss)
        {
            return CceNative.cce_model_train(_handle, dataset, (UIntPtr)maxEpochs, targetLoss);
        }

        public double TrainBatch(ref CceBatch batch)
        {
            return CceNative.cce_model_train_batch(_handle, ref batch);
        }

        public void InferBatch(ref CceBatch batch, int[] outLabels, float[] confs)
        {
            CceNative.cce_model_infer_batch(_handle, ref batch, outLabels, confs);
        }

        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                CceNative.cce_model_destroy(_handle);
                _handle = IntPtr.Zero;
            }
        }
    }

    public class CceDataset : IDisposable
    {
        private IntPtr _handle;

        public static CceDataset FromArrays(float[] inputs, float[] targets,
                                            int nSamples, int inDim, int outDim, int batchSize)
        {
            var ds = new CceDataset();
            if (CceNative.cce_dataset_from_arrays(out ds._handle,
                    inputs, targets, (UIntPtr)nSamples,
                    (UIntPtr)inDim, (UIntPtr)outDim, (UIntPtr)batchSize) != 0)
            {
                throw new Exception("Failed to create dataset");
            }
            return ds;
        }

        public static CceDataset WrapArrays(float[] inputs, float[] targets,
                                            int nSamples, int inDim, int outDim, int batchSize)
        {
            // zero-copy path for tile vecs / perceptual features (caller keeps arrays alive)
            var ds = new CceDataset();
            if (CceNative.cce_dataset_wrap_arrays(out ds._handle,
                    inputs, targets, (UIntPtr)nSamples,
                    (UIntPtr)inDim, (UIntPtr)outDim, (UIntPtr)batchSize) != 0)
            {
                throw new Exception("wrap failed");
            }
            return ds;
        }

        public bool NextBatch(out CceBatch batch)
        {
            batch = new CceBatch();
            return CceNative.cce_dataset_next_batch(_handle, ref batch) == 0;
        }

        public void Reset()
        {
            CceNative.cce_dataset_reset(_handle);
        }

        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                CceNative.cce_dataset_destroy(_handle);
                _handle = IntPtr.Zero;
            }
        }
    }
}
