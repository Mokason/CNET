using System;

namespace CNET.Cce;

public enum CceEarlyStoppingMonitor
{
    TrainLoss,
    ValidationLoss,
    ValidationAccuracy,
}

/// <summary>
/// Stateful managed early-stopping helper for <see cref="CceCallbacks.OnEpochEnd"/>.
/// This is host-side training control; the native CCE learner remains unchanged.
/// </summary>
public sealed class CceEarlyStopping
{
    private readonly bool _maximize;
    private int _epochsWithoutImprovement;

    public CceEarlyStopping(
        int patience = 5,
        double minDelta = 0.0,
        CceEarlyStoppingMonitor monitor = CceEarlyStoppingMonitor.ValidationLoss)
    {
        if (patience <= 0) throw new ArgumentOutOfRangeException(nameof(patience));
        if (minDelta < 0.0) throw new ArgumentOutOfRangeException(nameof(minDelta));

        Patience = patience;
        MinDelta = minDelta;
        Monitor = monitor;
        _maximize = monitor == CceEarlyStoppingMonitor.ValidationAccuracy;
    }

    public int Patience { get; }
    public double MinDelta { get; }
    public CceEarlyStoppingMonitor Monitor { get; }
    public bool ShouldStop { get; private set; }
    public int? BestEpoch { get; private set; }
    public double? BestValue { get; private set; }

    /// <summary>
    /// Callback target for <see cref="CceCallbacks.OnEpochEnd"/>. Returns false once patience is exhausted.
    /// </summary>
    public bool OnEpochEnd(EpochInfo info)
    {
        double value = GetMonitoredValue(info);
        if (!double.IsFinite(value))
        {
            _epochsWithoutImprovement++;
            ShouldStop = _epochsWithoutImprovement >= Patience;
            return !ShouldStop;
        }

        if (IsImprovement(value))
        {
            BestValue = value;
            BestEpoch = info.Epoch;
            _epochsWithoutImprovement = 0;
            return true;
        }

        _epochsWithoutImprovement++;
        ShouldStop = _epochsWithoutImprovement >= Patience;
        return !ShouldStop;
    }

    private bool IsImprovement(double value)
    {
        if (BestValue is null) return true;

        return _maximize
            ? value > BestValue.Value + MinDelta
            : value < BestValue.Value - MinDelta;
    }

    private double GetMonitoredValue(EpochInfo info) =>
        Monitor switch
        {
            CceEarlyStoppingMonitor.TrainLoss => info.TrainLoss,
            CceEarlyStoppingMonitor.ValidationLoss => info.ValLoss
                ?? throw new InvalidOperationException("Validation loss is not available. Pass a validation dataset to Fit/FitHistory or monitor TrainLoss."),
            CceEarlyStoppingMonitor.ValidationAccuracy => info.ValAccuracy
                ?? throw new InvalidOperationException("Validation accuracy is not available. Pass a validation dataset to Fit/FitHistory or monitor TrainLoss."),
            _ => throw new ArgumentOutOfRangeException(nameof(Monitor)),
        };
}
