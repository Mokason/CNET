using System.Text;

namespace CNET.Cce.Llm;

/// <summary>
/// Filters a live token stream so that action generations (RECALL/READ/CALC/
/// TOOL) never reach the viewer, while normal answers still stream live. The
/// deliberation loop generates several times per turn — action requests, the
/// real answer, auto-continue chunks — and each streams as it is produced;
/// without this, the raw "CALC: 738291 * 466517" scaffolding scrolls past
/// before the computed answer arrives.
/// </summary>
/// <remarks>
/// Per generation: buffer the opening until it can be classified. A prefix of
/// an action marker keeps buffering; a completed action marker suppresses the
/// whole generation; anything else flushes and streams live from then on. The
/// buffered prefix is at most a few characters, so real answers stream with no
/// perceptible delay. Reflection retries and rejected drafts are out of scope
/// — they look like content, and live streaming cannot be retracted.
/// </remarks>
public sealed class StreamGate(Action<string> sink)
{
    // The action markers, matched case-sensitively like the protocol parser.
    private static readonly string[] Markers = ["RECALL:", "READ:", "CALC:", "TOOL:"];

    private readonly Action<string> _sink = sink ?? throw new ArgumentNullException(nameof(sink));
    private readonly StringBuilder _buf = new();
    private bool _decided;
    private bool _suppress;

    /// <summary>Resets state for a new inner generation.</summary>
    public void BeginGeneration()
    {
        _buf.Clear();
        _decided = false;
        _suppress = false;
    }

    /// <summary>Feeds one streamed chunk; emits or withholds per the classification.</summary>
    public void Feed(string chunk)
    {
        if (chunk.Length == 0) return;
        if (_decided)
        {
            if (!_suppress) _sink(chunk);
            return;
        }

        _buf.Append(chunk);
        string t = _buf.ToString().TrimStart();
        if (t.Length == 0) return;   // still leading whitespace

        if (Markers.Any(m => t.StartsWith(m, StringComparison.Ordinal)))
        {
            _decided = true;
            _suppress = true;         // a real action — suppress the whole generation
            return;
        }
        if (Markers.Any(m => m.StartsWith(t, StringComparison.Ordinal)))
            return;                   // still could become an action — keep buffering

        // Definitely not an action: flush what we buffered and stream live.
        _decided = true;
        _suppress = false;
        _sink(_buf.ToString());
    }

    /// <summary>Generation ended: flush any leftover the classifier never decided
    /// on (a generation shorter than an action marker but not one).</summary>
    public void EndGeneration()
    {
        if (!_decided && _buf.Length > 0)
        {
            _decided = true;
            _sink(_buf.ToString());
        }
    }
}
