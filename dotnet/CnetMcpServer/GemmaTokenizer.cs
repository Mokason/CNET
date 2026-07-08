// Vendored from AICIMO_Lib/Gemma4/GemmaTokenizer.cs (bit-identical Gemma 4
// SentencePiece-BPE encode/decode). The CNET certified base soul_gemma4v2
// names its units acq_tk<id>q<id> by GEMMA VOCAB ID, so claim verification
// needs the same tokenizer that minted those ids. Keep in sync with AICIMO.
namespace Aicimo.Gemma4;

using System.Text;
using System.Text.Json;

/// <summary>
/// Gemma 4 tokenizer — a SentencePiece-style BPE wrapped in the
/// Hugging Face tokenizers JSON schema. Specifically:
///   • Normalizer: replace every ASCII space ' ' (U+0020) with the
///     SentencePiece word-boundary marker '▁' (U+2581).
///   • Pre-tokenizer: identity for our purposes (the Split('▁',
///     MergedWithPrevious) just keeps the word-boundary glued to the
///     following text).
///   • Model: byte-level BPE with rank-ordered merges. Vocab includes
///     literal byte-fallback pieces &lt;0x00&gt;..&lt;0xFF&gt; that catch any
///     codepoint whose UTF-8 bytes don't appear in vocab.
///   • Decoder: reverse the normalizer (▁ → space), then execute the
///     byte-fallback pieces (&lt;0xNN&gt; → raw byte), then fuse adjacent
///     pieces.
///   • Added tokens: 24 special tokens (&lt;bos&gt; / &lt;eos&gt; / &lt;pad&gt; /
///     &lt;unk&gt; / &lt;mask&gt; / role / multimodal markers / &lt;|tool&gt; /…) that
///     bypass BPE entirely and tokenize to their reserved id.
///
/// This implementation targets the bit-identical encode/decode path
/// that <c>tokenizer.encode(s, add_special_tokens=False)</c> produces
/// in Python. Post-processor (BOS injection / chat templating) is
/// handled separately — chat-template rendering lives in a future
/// <c>GemmaChatTemplate.cs</c>.
/// </summary>
public sealed class GemmaTokenizer
{
    public const char SentencePieceSpace = '▁';   // '▁'

    public IReadOnlyDictionary<string, int> Vocab { get; }
    public IReadOnlyList<string> IdToPiece { get; }
    public IReadOnlyList<AddedToken> AddedTokens { get; }
    public int? BosTokenId { get; }
    public int? EosTokenId { get; }
    public int? PadTokenId { get; }
    public int? UnkTokenId { get; }

    // Merge rank lookup: (left, right) -> priority (lower = applied first).
    private readonly Dictionary<MergePair, int> _mergeRank;
    // Pre-computed byte-fallback id for each of the 256 bytes; -1 if missing
    // from vocab (would be a corrupt tokenizer.json).
    private readonly int[] _byteFallbackId;
    // Specials indexed by their exact content string for fast scanning.
    private readonly Dictionary<string, int> _specialContentToId;

    private GemmaTokenizer(
        Dictionary<string, int> vocab,
        string[] idToPiece,
        List<AddedToken> added,
        Dictionary<MergePair, int> mergeRank,
        int? bos, int? eos, int? pad, int? unk)
    {
        Vocab = vocab;
        IdToPiece = idToPiece;
        AddedTokens = added;
        _mergeRank = mergeRank;
        BosTokenId = bos;
        EosTokenId = eos;
        PadTokenId = pad;
        UnkTokenId = unk;

        _byteFallbackId = new int[256];
        for (int b = 0; b < 256; b++)
        {
            string piece = $"<0x{b:X2}>";
            _byteFallbackId[b] = vocab.TryGetValue(piece, out var id) ? id : -1;
        }

        _specialContentToId = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var a in added)
            _specialContentToId[a.Content] = a.Id;
    }

    /// <summary>
    /// Loads <c>tokenizer.json</c> from the given model directory.
    /// </summary>
    public static GemmaTokenizer Load(string modelDir)
    {
        if (modelDir is null) throw new ArgumentNullException(nameof(modelDir));
        string path = Path.Combine(modelDir, "tokenizer.json");
        if (!File.Exists(path))
            throw new FileNotFoundException($"tokenizer.json not found in {modelDir}.");

        using var fs = File.OpenRead(path);
        using var doc = JsonDocument.Parse(fs);
        var root = doc.RootElement;

        // ----- model.vocab -----
        var modelEl = root.GetProperty("model");
        if (modelEl.GetProperty("type").GetString() != "BPE")
            throw new InvalidDataException("tokenizer.json model.type must be 'BPE' for Gemma 4.");

        var vocabEl = modelEl.GetProperty("vocab");
        var vocab = new Dictionary<string, int>(vocabEl.EnumerateObject().Count() + 32,
            StringComparer.Ordinal);
        int maxId = -1;
        foreach (var p in vocabEl.EnumerateObject())
        {
            int id = p.Value.GetInt32();
            vocab[p.Name] = id;
            if (id > maxId) maxId = id;
        }
        var idToPiece = new string[maxId + 1];
        foreach (var kv in vocab) idToPiece[kv.Value] = kv.Key;

        // ----- model.merges -----
        // HF stores merges as space-separated string pairs in priority
        // order (rank 0 = highest priority). Some newer dumps use a
        // 2-element array form ["a","b"]; handle both.
        var mergesEl = modelEl.GetProperty("merges");
        var mergeRank = new Dictionary<MergePair, int>(mergesEl.GetArrayLength());
        int rank = 0;
        foreach (var m in mergesEl.EnumerateArray())
        {
            string left, right;
            if (m.ValueKind == JsonValueKind.String)
            {
                string raw = m.GetString()!;
                int sp = raw.IndexOf(' ');
                if (sp < 0)
                    throw new InvalidDataException(
                        $"BPE merge rank {rank} is malformed: '{raw}'.");
                left = raw.Substring(0, sp);
                right = raw.Substring(sp + 1);
            }
            else if (m.ValueKind == JsonValueKind.Array && m.GetArrayLength() == 2)
            {
                left = m[0].GetString() ?? throw new InvalidDataException("merge null");
                right = m[1].GetString() ?? throw new InvalidDataException("merge null");
            }
            else
            {
                throw new InvalidDataException(
                    $"BPE merge rank {rank} is in an unrecognized form.");
            }
            // Ties: keep the first occurrence (lower rank).
            var key = new MergePair(left, right);
            if (!mergeRank.ContainsKey(key)) mergeRank[key] = rank;
            rank++;
        }

        // ----- added_tokens -----
        var added = new List<AddedToken>();
        if (root.TryGetProperty("added_tokens", out var atEl) && atEl.ValueKind == JsonValueKind.Array)
        {
            foreach (var a in atEl.EnumerateArray())
            {
                added.Add(new AddedToken(
                    Id: a.GetProperty("id").GetInt32(),
                    Content: a.GetProperty("content").GetString() ?? "",
                    Special: a.TryGetProperty("special", out var sp) && sp.GetBoolean(),
                    SingleWord: a.TryGetProperty("single_word", out var sw) && sw.GetBoolean(),
                    LStrip: a.TryGetProperty("lstrip", out var ls) && ls.GetBoolean(),
                    RStrip: a.TryGetProperty("rstrip", out var rs) && rs.GetBoolean(),
                    Normalized: a.TryGetProperty("normalized", out var nm) && nm.GetBoolean()
                ));
            }
        }

        // ----- special-token ids (BOS / EOS / PAD / UNK) -----
        // Mirror Python convention: pull from added_tokens by canonical
        // names. Gemma uses lowercase angle-bracket forms.
        int? Lookup(string name) =>
            vocab.TryGetValue(name, out var id) ? id : (int?)null;
        int? bos = Lookup("<bos>");
        int? eos = Lookup("<eos>");
        int? pad = Lookup("<pad>");
        int? unk = Lookup("<unk>");

        return new GemmaTokenizer(vocab, idToPiece, added, mergeRank, bos, eos, pad, unk);
    }

    // ----------------------------------------------------------------
    // Encoding
    // ----------------------------------------------------------------

    /// <summary>
    /// Encodes a string to token ids using the same logic as
    /// <c>tokenizer.encode(text, add_special_tokens=False)</c> in Python.
    /// Special tokens embedded in the text (e.g. literal "&lt;bos&gt;") are
    /// recognized and emitted as their reserved id, bypassing BPE.
    /// </summary>
    public List<int> Encode(string text)
    {
        var result = new List<int>(Math.Max(8, text.Length / 4));
        if (text.Length == 0) return result;

        // Step 1: scan for special-token boundaries. Each special-content
        // match emits its id directly; the spans between matches go
        // through the normal BPE path.
        int cursor = 0;
        while (cursor < text.Length)
        {
            (int hit, string content) = FindNextSpecial(text, cursor);
            int chunkEnd = hit < 0 ? text.Length : hit;

            if (chunkEnd > cursor)
                EncodeBpeChunk(text.AsSpan(cursor, chunkEnd - cursor), result);

            if (hit < 0) break;
            result.Add(_specialContentToId[content]);
            cursor = hit + content.Length;
        }
        return result;
    }

    private (int Position, string Content) FindNextSpecial(string text, int from)
    {
        // Linear scan: small fixed table (~24 entries) so this is cheap.
        int best = -1;
        string bestContent = "";
        foreach (var sp in AddedTokens)
        {
            if (sp.Content.Length == 0) continue;
            int idx = text.IndexOf(sp.Content, from, StringComparison.Ordinal);
            if (idx < 0) continue;
            if (best < 0 || idx < best || (idx == best && sp.Content.Length > bestContent.Length))
            {
                best = idx;
                bestContent = sp.Content;
            }
        }
        return (best, bestContent);
    }

    private void EncodeBpeChunk(ReadOnlySpan<char> chunk, List<int> outIds)
    {
        // Step 2: normalize (' ' → '▁').
        var normalized = new StringBuilder(chunk.Length);
        foreach (var c in chunk) normalized.Append(c == ' ' ? SentencePieceSpace : c);
        string norm = normalized.ToString();
        if (norm.Length == 0) return;

        // Step 3: pre-tokenize. Gemma's pre_tokenizer is
        // Split(pattern=' ', behavior=MergedWithPrevious). The split
        // pattern is the literal ASCII space, but the normalizer
        // already converted every ' ' to '▁', so there is nothing left
        // to split on — the pre-tokenizer is a no-op and BPE runs on
        // the entire normalized chunk as one word. This is what makes
        // multi-space runs collapse into pre-merged vocab pieces like
        // '▁▁' (138) and '▁▁▁' (139): all '▁'s are visible to BPE in
        // one pass and the high-priority space-on-space merges fire
        // before any '▁word' merges that would split them apart.
        EncodeBpeWord(norm, outIds);
    }

    private void EncodeBpeWord(string word, List<int> outIds)
    {
        if (word.Length == 0) return;

        // Initialise the working sequence as one piece per Unicode
        // codepoint (not per UTF-16 code unit), so surrogate pairs stay
        // glued together at the start of BPE.
        var pieces = new List<string>(word.Length);
        int idx = 0;
        while (idx < word.Length)
        {
            int cp = char.ConvertToUtf32(word, idx);
            int len = char.IsHighSurrogate(word[idx]) ? 2 : 1;
            pieces.Add(word.Substring(idx, len));
            idx += len;
        }

        // Iteratively apply the highest-priority adjacent merge.
        while (pieces.Count >= 2)
        {
            int bestRank = int.MaxValue;
            int bestIdx = -1;
            for (int j = 0; j < pieces.Count - 1; j++)
            {
                if (_mergeRank.TryGetValue(new MergePair(pieces[j], pieces[j + 1]), out var r) &&
                    r < bestRank)
                {
                    bestRank = r;
                    bestIdx = j;
                }
            }
            if (bestIdx < 0) break;
            pieces[bestIdx] = pieces[bestIdx] + pieces[bestIdx + 1];
            pieces.RemoveAt(bestIdx + 1);
        }

        // Emit ids; pieces not in vocab fall back to per-byte <0xNN>.
        foreach (var p in pieces)
        {
            if (Vocab.TryGetValue(p, out var id))
            {
                outIds.Add(id);
            }
            else
            {
                EmitByteFallback(p, outIds);
            }
        }
    }

    private void EmitByteFallback(string piece, List<int> outIds)
    {
        var bytes = Encoding.UTF8.GetBytes(piece);
        foreach (var b in bytes)
        {
            int id = _byteFallbackId[b];
            if (id < 0)
            {
                // Fall back to <unk> if even byte-fallback is missing
                // (shouldn't happen for Gemma's vocab).
                if (UnkTokenId is int unk) outIds.Add(unk);
            }
            else
            {
                outIds.Add(id);
            }
        }
    }

    // ----------------------------------------------------------------
    // Decoding
    // ----------------------------------------------------------------

    /// <summary>
    /// Decodes a list of token ids back to a string, mirroring
    /// <c>tokenizer.decode(ids, skip_special_tokens=False)</c>.
    /// Implements the tokenizer.json decoder pipeline in order:
    /// Replace('▁' → ' ') · ByteFallback · Fuse.
    /// </summary>
    public string Decode(IReadOnlyList<int> ids, bool skipSpecialTokens = false)
    {
        if (ids.Count == 0) return string.Empty;

        // Pass 1: id → piece text, dropping specials if requested.
        var pieces = new List<string>(ids.Count);
        var specialIds = new HashSet<int>(AddedTokens.Where(a => a.Special).Select(a => a.Id));
        foreach (var id in ids)
        {
            if (skipSpecialTokens && specialIds.Contains(id)) continue;
            if (id < 0 || id >= IdToPiece.Count) continue;
            string p = IdToPiece[id];
            if (p is null) continue;
            pieces.Add(p);
        }

        // Pass 2: ByteFallback — collapse runs of <0xNN> pieces into the
        // raw UTF-8 bytes they encode, then UTF-8 decode that run.
        // Non-byte-fallback pieces pass through verbatim. The Fuse step
        // is implicit because we accumulate everything into one buffer.
        var sb = new StringBuilder(pieces.Count * 4);
        var byteBuf = new List<byte>(64);

        void FlushBytes()
        {
            if (byteBuf.Count == 0) return;
            sb.Append(Encoding.UTF8.GetString(byteBuf.ToArray()));
            byteBuf.Clear();
        }

        foreach (var p in pieces)
        {
            if (TryParseByteFallback(p, out byte b))
            {
                byteBuf.Add(b);
            }
            else
            {
                FlushBytes();
                sb.Append(p);
            }
        }
        FlushBytes();

        // Pass 3: Replace '▁' → ' '.
        sb.Replace(SentencePieceSpace, ' ');
        return sb.ToString();
    }

    private static bool TryParseByteFallback(string piece, out byte b)
    {
        // Pattern: "<0xHH>" exactly, hex digits case-insensitive.
        b = 0;
        if (piece.Length != 6) return false;
        if (piece[0] != '<' || piece[1] != '0' || (piece[2] != 'x' && piece[2] != 'X') ||
            piece[5] != '>') return false;
        int Hi = ParseHex(piece[3]);
        int Lo = ParseHex(piece[4]);
        if (Hi < 0 || Lo < 0) return false;
        b = (byte)((Hi << 4) | Lo);
        return true;
    }

    private static int ParseHex(char c) => c switch
    {
        >= '0' and <= '9' => c - '0',
        >= 'a' and <= 'f' => 10 + (c - 'a'),
        >= 'A' and <= 'F' => 10 + (c - 'A'),
        _ => -1,
    };

    // ----------------------------------------------------------------
    // Supporting types
    // ----------------------------------------------------------------

    public readonly record struct AddedToken(
        int Id,
        string Content,
        bool Special,
        bool SingleWord,
        bool LStrip,
        bool RStrip,
        bool Normalized);

    private readonly record struct MergePair(string Left, string Right);
}
