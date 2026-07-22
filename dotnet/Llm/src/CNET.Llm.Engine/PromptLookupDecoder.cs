using System.Buffers;
using System.Diagnostics;
using CNET.Llm.Core.Attention;
using CNET.Llm.Core.Models;
using CNET.Llm.Core.Tensors;

namespace CNET.Llm.Engine;

/// <summary>
/// Speculative decoding with no draft model: candidates come from
/// <see cref="PromptLookupDrafter"/> and are verified by one batched target pass.
/// </summary>
/// <remarks>
/// Greedy only, matching <see cref="SpeculativeDecoder"/>. Accepting a proposed
/// token exactly when it equals the target's argmax means the emitted sequence is
/// identical to plain greedy decoding — this changes speed, never output.
/// <para>
/// Unlike model-based speculation there is no draft KV-cache and no second set of
/// weights, so a round costs one target forward whether it proposes one token or
/// ten. That is what makes it viable when decode is bandwidth-bound.
/// </para>
/// </remarks>
public sealed class PromptLookupDecoder
{
    private readonly PromptLookupDrafter _drafter;

    /// <param name="drafter">Supplies candidates from the context.</param>
    public PromptLookupDecoder(PromptLookupDrafter drafter)
        => _drafter = drafter ?? throw new ArgumentNullException(nameof(drafter));

    /// <summary>
    /// Proposes from context and verifies in one pass.
    /// </summary>
    /// <param name="targetModel">Model used for verification.</param>
    /// <param name="kvCache">Target KV-cache; rolled back to the accepted end.</param>
    /// <param name="sequence">Prompt plus everything generated so far.</param>
    /// <param name="position">Sequence position of the next token to emit.</param>
    /// <param name="vocabSize">Target vocabulary size.</param>
    /// <param name="outputBuffer">Receives accepted tokens. Must hold <see cref="PromptLookupDrafter.MaxCandidates"/> + 1.</param>
    /// <returns>
    /// Accepted count and timings. Zero accepted means no n-gram match was found
    /// and the caller should take a normal single-token step.
    /// </returns>
    public PromptLookupResult DraftAndVerify(
        IModel targetModel,
        IKvCache kvCache,
        ReadOnlySpan<int> sequence,
        int position,
        int vocabSize,
        Span<int> outputBuffer)
    {
        ArgumentNullException.ThrowIfNull(targetModel);
        ArgumentNullException.ThrowIfNull(kvCache);

        int maxCandidates = _drafter.MaxCandidates;
        if (outputBuffer.Length < maxCandidates + 1)
            throw new ArgumentException(
                $"outputBuffer must hold at least {maxCandidates + 1} tokens", nameof(outputBuffer));
        if (sequence.Length == 0)
            return default;

        int[] rented = ArrayPool<int>.Shared.Rent(maxCandidates);
        try
        {
            var candidates = rented.AsSpan(0, maxCandidates);
            long draftStart = Stopwatch.GetTimestamp();
            int k = _drafter.Draft(sequence, candidates);
            long draftTicks = Stopwatch.GetTimestamp() - draftStart;

            // No match: let the caller decode normally rather than burn a pass.
            if (k == 0)
                return new PromptLookupResult(0, 0, draftTicks, 0);

            // Verify [lastToken, c0..c_{k-1}]: logits at slot i predict what
            // follows that slot, so slot i is the check for candidate i and the
            // final slot supplies a free bonus token when everything matched.
            int verifyLen = k + 1;
            int room = kvCache.MaxLength - position;
            if (room < 1)
                return new PromptLookupResult(0, k, draftTicks, 0);
            verifyLen = Math.Min(verifyLen, room);
            k = verifyLen - 1;

            Span<int> tokens = verifyLen <= 16 ? stackalloc int[verifyLen] : new int[verifyLen];
            Span<int> positions = verifyLen <= 16 ? stackalloc int[verifyLen] : new int[verifyLen];

            tokens[0] = sequence[^1];
            positions[0] = position;
            for (int i = 0; i < k; i++)
            {
                tokens[i + 1] = candidates[i];
                positions[i + 1] = position + i + 1;
            }

            long verifyStart = Stopwatch.GetTimestamp();
            using ITensor logits = targetModel.Forward(tokens, positions, deviceId: -1, kvCache);
            long verifyTicks = Stopwatch.GetTimestamp() - verifyStart;

            int accepted = 0;
            unsafe
            {
                float* basePtr = (float*)logits.DataPointer;
                for (int i = 0; i < k; i++)
                {
                    int argmax = ArgMax(new ReadOnlySpan<float>(basePtr + (long)i * vocabSize, vocabSize));
                    if (argmax == candidates[i])
                    {
                        outputBuffer[accepted++] = argmax;
                        continue;
                    }

                    // First disagreement: the target's own choice is still correct
                    // greedy output, so keep it and stop.
                    outputBuffer[accepted++] = argmax;
                    kvCache.Rollback(position + accepted);
                    return new PromptLookupResult(accepted, k, draftTicks, verifyTicks);
                }

                // Every candidate matched — the last slot is a token for free.
                int bonus = ArgMax(new ReadOnlySpan<float>(basePtr + (long)k * vocabSize, vocabSize));
                outputBuffer[accepted++] = bonus;
            }

            kvCache.Rollback(position + accepted);
            return new PromptLookupResult(accepted, k, draftTicks, verifyTicks);
        }
        finally
        {
            ArrayPool<int>.Shared.Return(rented);
        }
    }

    private static int ArgMax(ReadOnlySpan<float> values)
    {
        int best = 0;
        float bestValue = values[0];
        for (int i = 1; i < values.Length; i++)
        {
            if (values[i] > bestValue)
            {
                bestValue = values[i];
                best = i;
            }
        }
        return best;
    }
}

/// <summary>Outcome of one prompt-lookup speculation round.</summary>
/// <param name="AcceptedCount">Tokens emitted this round; 0 means no match was found.</param>
/// <param name="DraftedCount">Tokens proposed, for measuring acceptance.</param>
/// <param name="DraftTicks">Time spent searching for an n-gram match.</param>
/// <param name="VerifyTicks">Time spent in the target forward pass.</param>
public readonly record struct PromptLookupResult(
    int AcceptedCount,
    int DraftedCount,
    long DraftTicks,
    long VerifyTicks);
