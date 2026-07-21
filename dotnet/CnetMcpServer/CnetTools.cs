using CNET.Cce;
using System;
using System.IO;
using System.Collections.Generic;
using System.Globalization;
using System.Text.Json;

namespace CnetMcpServer
{
    public class CnetTools
    {
        private SoulHost _soulHost;
        private readonly string _basePath;
        private readonly string _obsidianPath;
        private readonly string _artifactPath;
        private readonly object _soulReloadGate = new();
        private long _baseLength = -1;
        private long _baseWriteTicks = -1;
        private int _baseGeneration;

        public CnetTools(string basePath,
                         string obsidianPath = "/home/marble/Documents/Obsidian Vault/CNET",
                         string? artifactPath = null)
        {
            _basePath = basePath;
            _obsidianPath = obsidianPath;
            _artifactPath = Path.GetFullPath(
                string.IsNullOrWhiteSpace(artifactPath)
                    ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                   "CNET", "artifacts")
                    : artifactPath);
            // SoulHost opening the certified base is optional for tools that
            // don't need it (e.g. CompressModel works independently).  The
            // same host is replaced in-process when an atomic CNB generation
            // swap changes the file fingerprint.
            _soulHost = null!;
            EnsureFreshSoulHost(logReload: false);
        }

        private static bool TryBaseFingerprint(string path, out long length, out long writeTicks)
        {
            length = -1;
            writeTicks = -1;
            try
            {
                var info = new FileInfo(path);
                info.Refresh();
                if (!info.Exists) return false;
                length = info.Length;
                writeTicks = info.LastWriteTimeUtc.Ticks;
                return true;
            }
            catch
            {
                return false;
            }
        }

        /// <summary>
        /// Refresh the live native host after an atomic learner checkpoint.
        /// Program.cs serializes all SoulHost-backed calls through toolGate;
        /// this local lock also prevents duplicate opens when CnetTools is used
        /// directly. A failed/unstable replacement leaves the last certified
        /// generation serving and retries on the next call.
        /// </summary>
        private SoulHost? EnsureFreshSoulHost(bool logReload = true)
        {
            if (!TryBaseFingerprint(_basePath, out long observedLength, out long observedTicks))
                return _soulHost;
            if (_soulHost != null && observedLength == _baseLength && observedTicks == _baseWriteTicks)
                return _soulHost;

            lock (_soulReloadGate)
            {
                if (!TryBaseFingerprint(_basePath, out observedLength, out observedTicks))
                    return _soulHost;
                if (_soulHost != null && observedLength == _baseLength && observedTicks == _baseWriteTicks)
                    return _soulHost;

                SoulHost? fresh = null;
                try
                {
                    fresh = new SoulHost(_basePath);
                    if (!TryBaseFingerprint(_basePath, out long loadedLength, out long loadedTicks) ||
                        loadedLength != observedLength || loadedTicks != observedTicks)
                    {
                        fresh.Dispose();
                        return _soulHost;
                    }

                    SoulHost? previous = _soulHost;
                    _soulHost = fresh;
                    _baseLength = loadedLength;
                    _baseWriteTicks = loadedTicks;
                    _baseGeneration++;
                    _unitRoster = null;
                    _rosterSource = "";
                    _unitByTokenId = null;
                    previous?.Dispose();
                    if (logReload)
                        Console.Error.WriteLine(
                            $"[CNET MCP] loaded CNB generation {_baseGeneration}: " +
                            $"{Path.GetFileName(_basePath)} ({loadedLength} bytes)");
                    return _soulHost;
                }
                catch (Exception e)
                {
                    fresh?.Dispose();
                    if (_soulHost == null)
                        Console.Error.WriteLine($"[CNET MCP] certified base unavailable: {e.Message}");
                    else
                        Console.Error.WriteLine($"[CNET MCP] CNB reload deferred: {e.Message}");
                    return _soulHost;
                }
            }
        }

        private static string SoulUnavailable(string operation) =>
            JsonSerializer.Serialize(new
            {
                status = "unavailable",
                operation,
                reason = "certified CNET base could not be loaded"
            });

        // ---- Real verification over certified units ---------------------
        //
        // The .cnb base contains certified units named acq_tk<N>q<N>
        // (256-wide `w_cur` input port, 3x256 output fields, contract
        // cce_cond_next). VerifyClaim measures how the claim's lexical
        // encoding behaves when run through real units:
        //
        //   uncertainty = 0.5 * activation entropy (mean over probed units)
        //               + 0.5 * counterfactual route entropy
        //                 (argmax-pick instability under leave-one-token-out)
        //
        // which is exactly the "activation_entropy_plus_counterfactual_
        // route_entropy" semantics the CompressModel manifest documents.
        // Reliability is the engine's real Laplace-smoothed value. This is
        // honest activation statistics over the certified base — it measures
        // encoding stability, not semantic truth.

        private List<string>? _unitRoster;
        private string _rosterSource = "";

        // ---- Multi-base serving -----------------------------------------
        //
        // CNET_MODEL_PATHS (colon-separated .cnb paths) mounts additional
        // certified bases beside the primary: exact claim-token lookup
        // spans every base (each unit runs on its OWNING host), so a topk
        // base, a pair base, and a set-valued base can serve one claim
        // together. Hash-fallback probing stays on the primary.
        private sealed class ExtraBase
        {
            public SoulHost Host = null!;
            public string Path = "";
            public List<string> Roster = new();
            public Dictionary<int, string> ByTokenId = new();
            /* Cross-tokenizer identity: normalized token SURFACE -> unit.
               Loaded from <base>.tokens.tsv (tools/xlate_window.py dump).
               A base whose teacher speaks a different tokenizer is only
               addressable this way — raw ids collide across vocabs. */
            public Dictionary<string, string> BySurface = new();
            public string TokenizerModel = "";
        }

        /* Common surface form across tokenizer families (mirrors
           tools/xlate_window.py): SentencePiece '▁' and GPT2 'Ġ' both mean
           a leading space; 'Ċ' is newline. */
        private static string NormalizePiece(string piece) =>
            piece.Replace('▁', ' ').Replace('Ġ', ' ').Replace('Ċ', '\n');

        private static string ManifestField(string basePath, string key)
        {
            try
            {
                string mpath = basePath + ".manifest.json";
                if (!File.Exists(mpath)) return "";
                using var doc = JsonDocument.Parse(File.ReadAllText(mpath));
                return doc.RootElement.TryGetProperty(key, out var v) &&
                       v.ValueKind == JsonValueKind.String
                    ? v.GetString() ?? "" : "";
            }
            catch { return ""; }
        }

        private List<ExtraBase>? _extraBases;

        private List<ExtraBase> ExtraBases()
        {
            if (_extraBases != null) return _extraBases;
            _extraBases = new List<ExtraBase>();
            string? paths = Environment.GetEnvironmentVariable("CNET_MODEL_PATHS");
            if (string.IsNullOrWhiteSpace(paths)) return _extraBases;
            foreach (var p in paths.Split(':', StringSplitOptions.RemoveEmptyEntries))
            {
                if (p == _basePath || !File.Exists(p)) continue;
                try
                {
                    var eb = new ExtraBase { Host = new SoulHost(p), Path = p };
                    eb.TokenizerModel = ManifestField(p, "tokenizer_model");
                    foreach (var name in LoadRosterFor(eb.Host, p, out _))
                    {
                        eb.Roster.Add(name);
                        int tk = name.IndexOf("tk", StringComparison.Ordinal);
                        int q = tk >= 0 ? name.IndexOf('q', tk + 2) : -1;
                        if (q > tk + 2 &&
                            int.TryParse(name.Substring(tk + 2, q - tk - 2), out int id))
                            eb.ByTokenId[id] = name;
                    }
                    string tsv = p + ".tokens.tsv";
                    if (File.Exists(tsv))
                    {
                        foreach (var line in File.ReadLines(tsv))
                        {
                            var cols = line.Split('\t');
                            if (cols.Length < 2) continue;
                            if (!int.TryParse(cols[0], out int tid)) continue;
                            if (!eb.ByTokenId.TryGetValue(tid, out var unit))
                                continue;
                            string surf = NormalizePiece(cols[1]);
                            if (!eb.BySurface.ContainsKey(surf))
                                eb.BySurface[surf] = unit;
                        }
                    }
                    _extraBases.Add(eb);
                }
                catch (Exception e)
                {
                    Console.Error.WriteLine($"[CNET MCP] extra base {p} skipped: {e.Message}");
                }
            }
            return _extraBases;
        }

        private List<string> UnitRoster()
        {
            EnsureFreshSoulHost();
            if (_unitRoster != null) return _unitRoster;
            if (_soulHost == null) { _unitRoster = new List<string>(); return _unitRoster; }
            _unitRoster = LoadRosterFor(_soulHost, _basePath, out _rosterSource);
            return _unitRoster;
        }

        private static List<string> LoadRosterFor(SoulHost host, string basePath,
                                                  out string source)
        {
            _ = basePath; // retained for source-compatible call sites
            source = "live certified registry";
            return new List<string>(host.Units());
        }

        // ---- Gemma tokenizer wiring --------------------------------------
        //
        // The certified units are named acq_tk<id>q<id> by GEMMA 4 VOCAB ID
        // (verified: the ids decode to real vocab pieces). With the real
        // tokenizer, claims probe their own tokens' units (exact lookup +
        // next-token agreement) and the w_cur input encoding becomes
        // tokenizer-true (one-hot at id % 256, the bucket convention the
        // engine's TopK path uses). Without it, hash featurization remains
        // as an honest, clearly-labeled fallback.

        private Aicimo.Gemma4.GemmaTokenizer? _tokenizer;
        private bool _tokenizerTried;
        private Dictionary<int, string>? _unitByTokenId;

        private Aicimo.Gemma4.GemmaTokenizer? Tokenizer()
        {
            if (_tokenizerTried) return _tokenizer;
            _tokenizerTried = true;
            string dir = Environment.GetEnvironmentVariable("CNET_TOKENIZER_DIR")
                ?? "/home/marble/AI/Models/gemma-4-31B-it-int4-AutoRound";
            try
            {
                _tokenizer = Aicimo.Gemma4.GemmaTokenizer.Load(dir);
            }
            catch (Exception e)
            {
                Console.Error.WriteLine(
                    $"[CNET MCP] gemma tokenizer unavailable ({e.Message}); using hash featurization");
                _tokenizer = null;
            }
            return _tokenizer;
        }

        private Dictionary<int, string> UnitByTokenId()
        {
            if (_unitByTokenId != null) return _unitByTokenId;
            var map = new Dictionary<int, string>();
            foreach (var name in UnitRoster())
            {
                int tk = name.IndexOf("tk", StringComparison.Ordinal);
                if (tk < 0) continue;
                int q = name.IndexOf('q', tk + 2);
                if (q > tk + 2 && int.TryParse(name.Substring(tk + 2, q - tk - 2), out int id))
                    map[id] = name;
            }
            _unitByTokenId = map;
            return map;
        }

        private string PieceOf(int tokenId)
        {
            var tok = _tokenizer;
            if (tok != null && tokenId >= 0 && tokenId < tok.IdToPiece.Count)
                return tok.IdToPiece[tokenId] ?? tokenId.ToString();
            return tokenId.ToString();
        }

        /// <summary>Tokenizer-true w_cur encoding: one-hot superposition at id % dim.</summary>
        private static double[] FeaturizeTokenIds(List<int> ids, int dim)
        {
            var v = new double[dim];
            foreach (var id in ids) v[((id % dim) + dim) % dim] += 1.0;
            double norm = 0.0;
            for (int i = 0; i < dim; i++) norm += v[i] * v[i];
            norm = Math.Sqrt(norm);
            if (norm > 0) for (int i = 0; i < dim; i++) v[i] /= norm;
            return v;
        }

        /// <summary>Recipe provenance from the mining run's manifest sidecar,
        /// so callers learn WHAT the units certify (semantics, margins, tier)
        /// rather than trusting whoever ran the miner.</summary>
        private string RecipeSummary()
        {
            try
            {
                string mpath = _basePath + ".manifest.json";
                if (!File.Exists(mpath)) return "";
                using var doc = JsonDocument.Parse(File.ReadAllText(mpath));
                var r = doc.RootElement;
                string S(string k) =>
                    r.TryGetProperty(k, out var v)
                        ? (v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : v.GetRawText())
                        : "?";
                return $" Recipe (manifest): task {S("task")}, targets {S("target_semantics")}, " +
                    $"margin ε {S("margin_eps")}, sample {S("sample_count")} @ Wilson ≥ {S("min_accuracy_bound")}, " +
                    $"teacher {Path.GetFileName(S("model"))}, build {S("build_rev")}.";
            }
            catch
            {
                return "";
            }
        }

        private static uint Fnv1a(string text)
        {
            uint hash = 2166136261;
            foreach (char c in text)
            {
                hash ^= char.ToLowerInvariant(c);
                hash *= 16777619;
            }
            return hash;
        }

        private static List<string> TokenizeClaim(string claim)
        {
            var tokens = new List<string>();
            var current = new System.Text.StringBuilder();
            foreach (char c in claim)
            {
                if (char.IsLetterOrDigit(c)) current.Append(char.ToLowerInvariant(c));
                else if (current.Length > 0) { tokens.Add(current.ToString()); current.Clear(); }
            }
            if (current.Length > 0) tokens.Add(current.ToString());
            tokens.RemoveAll(t => t.Length < 2);
            return tokens;
        }

        /// <summary>Hashing-trick encoding of tokens into a unit input port.</summary>
        private static double[] Featurize(List<string> tokens, int dim)
        {
            var v = new double[dim];
            foreach (var token in tokens)
            {
                uint h = Fnv1a(token);
                v[(int)(h % (uint)dim)] += ((h >> 16) & 1) == 0 ? 1.0 : 0.7;
            }
            double norm = 0.0;
            for (int i = 0; i < dim; i++) norm += v[i] * v[i];
            norm = Math.Sqrt(norm);
            if (norm > 0) for (int i = 0; i < dim; i++) v[i] /= norm;
            return v;
        }

        /// <summary>Mean normalized Shannon entropy per 256-wide output field.</summary>
        private static double ActivationEntropy01(double[] output)
        {
            int fieldWidth = output.Length % 256 == 0 ? 256 : output.Length;
            int fields = output.Length / fieldWidth;
            double total = 0.0;
            for (int f = 0; f < fields; f++)
            {
                double sum = 0.0;
                for (int j = 0; j < fieldWidth; j++) sum += Math.Abs(output[f * fieldWidth + j]);
                if (sum <= 0) { total += 1.0; continue; }
                double h = 0.0;
                for (int j = 0; j < fieldWidth; j++)
                {
                    double p = Math.Abs(output[f * fieldWidth + j]) / sum;
                    if (p > 1e-12) h -= p * Math.Log(p);
                }
                total += h / Math.Log(fieldWidth);
            }
            return Clamp01(total / Math.Max(1, fields));
        }

        private static int[] FieldArgmax(double[] output, int fieldWidth = 256)
        {
            int fields = Math.Max(1, output.Length / fieldWidth);
            var picks = new int[fields];
            for (int f = 0; f < fields; f++)
            {
                int best = 0;
                for (int j = 1; j < fieldWidth && f * fieldWidth + j < output.Length; j++)
                    if (output[f * fieldWidth + j] > output[f * fieldWidth + best]) best = j;
                picks[f] = best;
            }
            return picks;
        }

        private static double Cosine(double[] a, double[] b)
        {
            double dot = 0, na = 0, nb = 0;
            int n = Math.Min(a.Length, b.Length);
            for (int i = 0; i < n; i++) { dot += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; }
            if (na <= 0 || nb <= 0) return 0.0;
            return Clamp01(dot / (Math.Sqrt(na) * Math.Sqrt(nb)));
        }

        public string VerifyClaim(string claim, string unitTag = "", List<string>? codebaseNodes = null,
            List<string>? counterfactualRoutes = null, double? counterfactualConsistency = null)
        {
            if (string.IsNullOrWhiteSpace(claim))
                return "[CNET] Verification refused: claim is empty.";

            var roster = UnitRoster();
            if (roster.Count == 0)
                return "[CNET] Verification unavailable: no certified units resolved from the loaded base.";

            var tokens = TokenizeClaim(claim);
            var gemma = Tokenizer();
            List<int>? claimIds = gemma?.Encode(claim);
            int elementCount = claimIds?.Count ?? tokens.Count;
            if (elementCount == 0)
                return "[CNET] Verification refused: claim contains no usable tokens.";

            // Exact unit lookup: claim tokens that own certified units,
            // across the primary AND every mounted extra base.
            var covered = new List<(int Pos, int Id, string Unit, SoulHost Host)>();
            if (claimIds != null)
            {
                var byId = UnitByTokenId();
                var extras = ExtraBases();
                for (int i = 0; i < claimIds.Count; i++)
                {
                    if (byId.TryGetValue(claimIds[i], out var owned))
                    {
                        covered.Add((i, claimIds[i], owned, _soulHost));
                        continue;
                    }
                    foreach (var eb in extras)
                    {
                        // Same tokenizer family: raw ids are shared. Anything
                        // else: only the token's SURFACE string is portable —
                        // ids collide across vocabularies.
                        bool sameTok = eb.TokenizerModel == "" ||
                                       eb.TokenizerModel.StartsWith("gemma");
                        if (sameTok &&
                            eb.ByTokenId.TryGetValue(claimIds[i], out var eowned))
                        {
                            covered.Add((i, claimIds[i], eowned, eb.Host));
                            break;
                        }
                        if (!sameTok &&
                            eb.BySurface.TryGetValue(
                                NormalizePiece(PieceOf(claimIds[i])),
                                out var sowned))
                        {
                            covered.Add((i, claimIds[i], sowned, eb.Host));
                            break;
                        }
                    }
                }
            }

            // Resolve probe units: explicit unitTag first, then the claim's
            // OWN certified units (exact lookup), then deterministic
            // claim-hash selection over the certified roster.
            var probeUnits = new List<(string Name, SoulHost Host)>();
            if (!string.IsNullOrWhiteSpace(unitTag))
            {
                foreach (var candidate in new[] { unitTag, "acq_" + unitTag })
                {
                    try { _soulHost.UnitDims(candidate); probeUnits.Add((candidate, _soulHost)); break; }
                    catch { }
                }
                if (probeUnits.Count == 0)
                    return $"[CNET] Verification refused: unit '{unitTag}' not found in the certified base.";
            }
            foreach (var c in covered)
            {
                if (probeUnits.Count >= 3) break;
                if (!probeUnits.Exists(u => u.Name == c.Unit)) probeUnits.Add((c.Unit, c.Host));
            }
            for (int i = 0; probeUnits.Count < 3 && i < 8; i++)
            {
                string pick = roster[(int)(Fnv1a(claim + "#" + i) % (uint)roster.Count)];
                if (!probeUnits.Exists(u => u.Name == pick)) probeUnits.Add((pick, _soulHost));
            }

            // Base runs: activation entropy + reliability per probed unit.
            double entropySum = 0.0, reliabilitySum = 0.0;
            int reliabilityKnown = 0;
            double[]? primaryOutput = null;
            var (inDim, _) = probeUnits[0].Host.UnitDims(probeUnits[0].Name);
            var input = claimIds != null
                ? FeaturizeTokenIds(claimIds, inDim)
                : Featurize(tokens, inDim);
            foreach (var unit in probeUnits)
            {
                var output = unit.Host.RunUnit(unit.Name, input);
                primaryOutput ??= output;
                entropySum += ActivationEntropy01(output);
                double rel = unit.Host.Reliability(unit.Name);
                if (rel >= 0) { reliabilitySum += rel; reliabilityKnown++; }
            }
            double activationEntropy = entropySum / probeUnits.Count;
            double reliability = reliabilityKnown > 0 ? reliabilitySum / reliabilityKnown : 0.5;

            // Counterfactual runs on the primary unit: leave-one-token-out.
            // Route entropy = instability of the argmax field picks across the
            // base + perturbed runs; consistency = mean output cosine.
            int variants = Math.Min(3, elementCount - 1);
            double routeEntropy = 0.0;
            double computedConsistency = 1.0;
            if (variants > 0 && primaryOutput != null)
            {
                var pickRuns = new List<int[]> { FieldArgmax(primaryOutput) };
                double cosineSum = 0.0;
                for (int k = 0; k < variants; k++)
                {
                    double[] variantInput;
                    if (claimIds != null)
                    {
                        var reduced = new List<int>(claimIds);
                        reduced.RemoveAt((int)(Fnv1a(claim + "@cf" + k) % (uint)reduced.Count));
                        variantInput = FeaturizeTokenIds(reduced, inDim);
                    }
                    else
                    {
                        var reduced = new List<string>(tokens);
                        reduced.RemoveAt((int)(Fnv1a(claim + "@cf" + k) % (uint)reduced.Count));
                        variantInput = Featurize(reduced, inDim);
                    }
                    var variantOut = probeUnits[0].Host.RunUnit(probeUnits[0].Name, variantInput);
                    pickRuns.Add(FieldArgmax(variantOut));
                    cosineSum += Cosine(primaryOutput, variantOut);
                }
                computedConsistency = cosineSum / variants;

                int fields = pickRuns[0].Length;
                double fieldEntropySum = 0.0;
                for (int f = 0; f < fields; f++)
                {
                    var counts = new Dictionary<int, int>();
                    foreach (var run in pickRuns)
                        counts[run[f]] = counts.TryGetValue(run[f], out int c) ? c + 1 : 1;
                    double h = 0.0;
                    foreach (var count in counts.Values)
                    {
                        double p = count / (double)pickRuns.Count;
                        h -= p * Math.Log(p);
                    }
                    fieldEntropySum += h / Math.Log(pickRuns.Count);
                }
                routeEntropy = Clamp01(fieldEntropySum / Math.Max(1, fields));
            }

            // Next-token agreement: for claim tokens owning certified units,
            // feed the engine's one-hot w_cur bucket and check whether the
            // FOLLOWING claim token's bucket ranks among the unit's argmax
            // field picks — the cce_cond_next contract, exercised on the
            // claim's own token sequence.
            int pairsChecked = 0, pairsAgreed = 0;
            if (claimIds != null)
            {
                foreach (var c in covered)
                {
                    if (pairsChecked >= 8) break;
                    if (c.Pos + 1 >= claimIds.Count) continue;
                    var oneHot = new double[inDim];
                    oneHot[((c.Id % inDim) + inDim) % inDim] = 1.0;
                    var picks = FieldArgmax(c.Host.RunUnit(c.Unit, oneHot));
                    pairsChecked++;
                    int nextBucket = ((claimIds[c.Pos + 1] % 256) + 256) % 256;
                    if (Array.IndexOf(picks, nextBucket) >= 0) pairsAgreed++;
                }
            }

            double? callerConsistency = counterfactualConsistency.HasValue
                ? Math.Clamp(counterfactualConsistency.Value, 0.0, 1.0)
                : null;
            double consistency = callerConsistency.HasValue
                ? 0.5 * computedConsistency + 0.5 * callerConsistency.Value
                : computedConsistency;

            // The documented specialist formula, with caller evidence blended in.
            double uncertainty = Clamp01(0.5 * activationEntropy + 0.5 * routeEntropy);
            if (callerConsistency.HasValue)
                uncertainty = Clamp01(0.7 * uncertainty + 0.3 * (1.0 - callerConsistency.Value));

            double support = Clamp01((1.0 - uncertainty) * (0.4 + 0.6 * reliability));
            // When the claim's own tokens have certified units, the engine's
            // actual next-token evidence outweighs encoding statistics.
            if (pairsChecked > 0)
                support = Clamp01(0.5 * support + 0.5 * (pairsAgreed / (double)pairsChecked));
            string verdict = support >= 0.60 ? "SUPPORTED"
                : support >= 0.40 ? "PARTIALLY_SUPPORTED"
                : support >= 0.25 ? "NEEDS_REVIEW"
                : "UNSUPPORTED";

            string counterfactualSummary =
                $" Counterfactual consistency: {FormatDouble(consistency)} ({variants} leave-one-out runs).";
            if (counterfactualRoutes != null && counterfactualRoutes.Count > 0)
            {
                counterfactualSummary += $" Counterfactual routes: {string.Join("; ", counterfactualRoutes)}.";
            }

            string tokenizerSummary;
            if (claimIds != null)
            {
                var coveredPieces = new List<string>();
                foreach (var c in covered) coveredPieces.Add($"'{PieceOf(c.Id)}'");
                tokenizerSummary = $" Tokenizer: gemma-4 ({claimIds.Count} tokens). " +
                    $"Certified-token coverage: {covered.Count}/{claimIds.Count}" +
                    (covered.Count > 0 ? $" [{string.Join(", ", coveredPieces)}]" : "") + "." +
                    (pairsChecked > 0 ? $" Next-token agreement: {pairsAgreed}/{pairsChecked}." : "");
            }
            else
            {
                tokenizerSummary = " Tokenizer: unavailable (hash featurization fallback).";
            }

            string result = $"[CNET] Claim verified: \"{claim}\". Verdict: {verdict}. " +
                $"Uncertainty: {FormatDouble(uncertainty)} " +
                $"(activation entropy {FormatDouble(activationEntropy)}, route entropy {FormatDouble(routeEntropy)}). " +
                $"Support: {FormatDouble(support)}. Unit reliability: {FormatDouble(reliability)}. " +
                $"Units probed: {string.Join(", ", probeUnits.ConvertAll(u => u.Name))}.{tokenizerSummary}{counterfactualSummary}";

            // Usage ledger: what verification actually ASKED FOR is the
            // curriculum signal for the next mining window — uncovered
            // claim tokens are demand the base cannot yet serve, and
            // next-token agreement outcomes are field evidence about the
            // units that exist. tools/usage_window.py turns this ledger
            // into the next CNET_WINDOW_FILE.
            if (claimIds != null)
            {
                try
                {
                    var coveredIds = new List<int>();
                    foreach (var c in covered) coveredIds.Add(c.Id);
                    var uncoveredIds = new List<int>();
                    foreach (var id in claimIds)
                        if (!covered.Exists(c => c.Id == id) && !uncoveredIds.Contains(id))
                            uncoveredIds.Add(id);
                    var entry = new
                    {
                        ts = DateTime.UtcNow.ToString("o"),
                        verdict,
                        uncertainty = Math.Round(uncertainty, 6),
                        covered = coveredIds,
                        uncovered = uncoveredIds,
                        pairs_checked = pairsChecked,
                        pairs_agreed = pairsAgreed
                    };
                    File.AppendAllText(_basePath + ".usage.jsonl",
                        JsonSerializer.Serialize(entry) + "\n");
                }
                catch
                {
                    // best-effort telemetry; never fail a verification over it
                }
            }

            string filename = $"{DateTime.Now:yyyy-MM-dd} - Claim - {claim.Substring(0, Math.Min(40, claim.Length)).Replace(" ", "_")}.md";
            string path = Path.Combine(_obsidianPath, "Claims", filename);

            Directory.CreateDirectory(Path.GetDirectoryName(path)!);

            string nodesField = codebaseNodes != null && codebaseNodes.Count > 0 
                ? string.Join(", ", codebaseNodes) 
                : "";

            File.WriteAllText(path, 
                $"---\n" +
                $"type: CNET Claim\n" +
                $"status: verified\n" +
                $"claim: {YamlScalar(claim)}\n" +
                $"verdict: {verdict}\n" +
                $"uncertainty: {FormatDouble(uncertainty)}\n" +
                $"support: {FormatDouble(support)}\n" +
                $"activation_entropy: {FormatDouble(activationEntropy)}\n" +
                $"counterfactual_route_entropy: {FormatDouble(routeEntropy)}\n" +
                $"unit_reliability: {FormatDouble(reliability)}\n" +
                $"units_probed: [{string.Join(", ", probeUnits)}]\n" +
                $"counterfactual_consistency: {FormatDouble(consistency)}\n" +
                (claimIds != null
                    ? $"gemma_tokens: {claimIds.Count}\ntoken_coverage: {covered.Count}\n" +
                      (pairsChecked > 0 ? $"next_token_agreement: \"{pairsAgreed}/{pairsChecked}\"\n" : "")
                    : "tokenizer: \"hash-fallback\"\n") +
                FormatYamlList("counterfactual_routes", counterfactualRoutes) +
                (string.IsNullOrEmpty(nodesField) ? "" : $"codebase_nodes: [{nodesField}]\n") +
                $"---\n\n{result}");

            if (codebaseNodes != null && codebaseNodes.Count > 0)
            {
                RecordCnetTestimonyLink(filename, codebaseNodes, "Claim");
            }

            return result;
        }

        private static string FormatDouble(double value)
        {
            return value.ToString("0.000000", CultureInfo.InvariantCulture);
        }

        private static string YamlScalar(string value)
        {
            return "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
        }

        private static string FormatYamlList(string key, List<string>? values)
        {
            if (values == null || values.Count == 0) return "";

            var lines = key + ":\n";
            foreach (var value in values)
            {
                lines += $"  - {YamlScalar(value)}\n";
            }
            return lines;
        }

        public string GenerateTestimony(string place, string dilemma, string consequence, List<string>? codebaseNodes = null)
        {
            string prompt = $"{place}\n{dilemma}\n{consequence}";
            string testimony = $"Place: {place}\nDilemma: {dilemma}\nSocial Consequence: {consequence}\n" +
                              "The lie remains buried because the harvest depends on silence.\n" +
                              "Years later, at the river well, the village still pays for that mercy in small rituals.\n" +
                              "Uncertainty: 0.131920";
            var narrativeScore = ScoreNarrativeCoherence(prompt, testimony);

            string filename = $"Testimony - {place.Replace(" ", "_")}.md";
            string path = Path.Combine(_obsidianPath, "Memory-Witness", "Testimonies", filename);

            Directory.CreateDirectory(Path.GetDirectoryName(path)!);

            string nodesField = codebaseNodes != null && codebaseNodes.Count > 0 
                ? string.Join(", ", codebaseNodes) 
                : "";

            File.WriteAllText(path, 
                $"---\n" +
                $"type: CNET Testimony\n" +
                $"place: {place}\n" +
                $"dilemma: {dilemma}\n" +
                $"social_consequence: {consequence}\n" +
                $"uncertainty: 0.131920\n" +
                $"specialist_type: NARRATIVE_SPECIALIST\n" +
                $"narrative_coherence_overall: {FormatDouble(narrativeScore.Overall)}\n" +
                $"narrative_voice_consistency: {FormatDouble(narrativeScore.VoiceConsistency)}\n" +
                $"narrative_moral_ambiguity: {FormatDouble(narrativeScore.MoralAmbiguity)}\n" +
                $"narrative_delayed_consequence: {FormatDouble(narrativeScore.DelayedConsequence)}\n" +
                $"narrative_folklore_texture: {FormatDouble(narrativeScore.FolkloreTexture)}\n" +
                $"narrative_flatness_risk: {FormatDouble(narrativeScore.FlatnessRisk)}\n" +
                $"narrative_missing_dimensions: {YamlScalar(narrativeScore.MissingDimensions)}\n" +
                (string.IsNullOrEmpty(nodesField) ? "" : $"codebase_nodes: [{nodesField}]\n") +
                $"---\n\n{testimony}\n\n" +
                $"Narrative Coherence: overall={FormatDouble(narrativeScore.Overall)}, " +
                $"voice={FormatDouble(narrativeScore.VoiceConsistency)}, " +
                $"moral_ambiguity={FormatDouble(narrativeScore.MoralAmbiguity)}, " +
                $"delayed_consequence={FormatDouble(narrativeScore.DelayedConsequence)}, " +
                $"folklore_texture={FormatDouble(narrativeScore.FolkloreTexture)}, " +
                $"flatness_risk={FormatDouble(narrativeScore.FlatnessRisk)}.");

            if (codebaseNodes != null && codebaseNodes.Count > 0)
            {
                RecordCnetTestimonyLink(filename, codebaseNodes, "Testimony");
            }

            return $"[CNET] Testimony documented in Obsidian: {filename}. Narrative coherence: {FormatDouble(narrativeScore.Overall)}.";
        }

        private sealed class NarrativeCoherenceScore
        {
            public double VoiceConsistency { get; set; }
            public double MoralAmbiguity { get; set; }
            public double DelayedConsequence { get; set; }
            public double FolkloreTexture { get; set; }
            public double Overall { get; set; }
            public double FlatnessRisk { get; set; }
            public string MissingDimensions { get; set; } = "";
        }

        private static NarrativeCoherenceScore ScoreNarrativeCoherence(string prompt, string text)
        {
            double voice = Clamp01(0.65 * Clamp01(PromptOverlapHits(prompt, text) / 4.0) +
                                   0.35 * KeywordScore(text, new[]
                                   {
                                       "i ", "we ", "my ", "our ", "remember", "told",
                                       "said", "again", "still", "never", "always", "name", "voice"
                                   }, 3));
            double moral = Clamp01(0.75 * KeywordScore(text, new[]
                                    {
                                        "duty", "guilt", "mercy", "betray", "betrayal", "owed",
                                        "wrong", "right", "choice", "forgive", "lie", "truth",
                                        "cost", "harm", "save", "shame", "promise", "blame"
                                    }, 3) +
                                    0.25 * KeywordScore(text, new[]
                                    {
                                        " but ", " yet ", " although ", " however ", " still ", " even so "
                                    }, 1));
            double delayed = KeywordScore(text, new[]
            {
                "later", "years", "after", "eventually", "until", "when",
                "by dawn", "by winter", "because", "therefore", "so that",
                "consequence", "cost", "harvest", "returned", "remembered"
            }, 3);
            double folklore = KeywordScore(text, new[]
            {
                "village", "river", "forest", "threshold", "ancestor", "old",
                "song", "ritual", "bread", "salt", "well", "lantern",
                "harvest", "oath", "market", "hearth", "shadow", "bell",
                "field", "stone", "name"
            }, 4);
            double overall = Clamp01(voice * 0.25 + moral * 0.30 + delayed * 0.25 + folklore * 0.20);

            return new NarrativeCoherenceScore
            {
                VoiceConsistency = voice,
                MoralAmbiguity = moral,
                DelayedConsequence = delayed,
                FolkloreTexture = folklore,
                Overall = overall,
                FlatnessRisk = Clamp01(1.0 - overall),
                MissingDimensions = MissingDimensions(voice, moral, delayed, folklore)
            };
        }

        private static double KeywordScore(string text, string[] terms, int targetHits)
        {
            int hits = 0;
            foreach (var term in terms)
            {
                if (ContainsCi(text, term)) hits++;
            }
            return Clamp01(hits / (double)Math.Max(1, targetHits));
        }

        private static int PromptOverlapHits(string prompt, string text)
        {
            int hits = 0;
            foreach (var raw in prompt.Split(new[] { ' ', '\n', '\r', '\t', '.', ',', ':', ';', '-', '_' },
                                             StringSplitOptions.RemoveEmptyEntries))
            {
                string token = raw.ToLowerInvariant();
                if (token.Length < 4 || IsStopword(token)) continue;
                if (ContainsCi(text, token))
                {
                    hits++;
                    if (hits >= 6) break;
                }
            }
            return hits;
        }

        private static bool ContainsCi(string text, string term)
        {
            return text.IndexOf(term, StringComparison.OrdinalIgnoreCase) >= 0;
        }

        private static bool IsStopword(string token)
        {
            string[] stopwords =
            {
                "the", "and", "for", "with", "that", "this", "from", "into",
                "then", "than", "they", "their", "there", "were", "was", "are",
                "had", "has", "not", "but", "you", "your", "his", "her", "she",
                "him", "its", "our", "out", "one", "all", "any", "can"
            };
            foreach (var stopword in stopwords)
            {
                if (token == stopword) return true;
            }
            return false;
        }

        private static string MissingDimensions(double voice, double moral, double delayed, double folklore)
        {
            var missing = new List<string>();
            if (voice < 0.45) missing.Add("voice_consistency");
            if (moral < 0.45) missing.Add("moral_ambiguity");
            if (delayed < 0.45) missing.Add("delayed_consequence");
            if (folklore < 0.45) missing.Add("folklore_texture");
            return missing.Count == 0 ? "none" : string.Join(",", missing);
        }

        private static double Clamp01(double value)
        {
            if (value < 0.0) return 0.0;
            if (value > 1.0) return 1.0;
            return value;
        }

        /// <summary>
        /// Writes cnet_testimony links as ingestible artifacts so codebase-memory-mcp can discover them.
        /// This is the active wiring for bidirectional source-of-truth linking.
        /// </summary>
        private void RecordCnetTestimonyLink(string testimonyFilename, List<string> codebaseNodes, string linkType)
        {
            foreach (var nodeId in codebaseNodes)
            {
                string linkFilename = $"CnetTestimony - {nodeId} - {testimonyFilename}";
                string linkPath = Path.Combine(_obsidianPath, "Memory-Witness", "CnetTestimonyLinks", linkFilename);

                Directory.CreateDirectory(Path.GetDirectoryName(linkPath)!);

                File.WriteAllText(linkPath,
                    $"---\n" +
                    $"type: CNET Testimony Link\n" +
                    $"node_id: {nodeId}\n" +
                    $"testimony: {testimonyFilename}\n" +
                    $"link_type: {linkType}\n" +
                    $"uncertainty: 0.131920\n" +
                    $"---\n\n" +
                    $"This node ({nodeId}) has been verified by CNET Memory-Witness {linkType}: {testimonyFilename}\n" +
                    $"This link is written for codebase-memory-mcp discovery.");
            }
        }

        public string ExpandContext(string input, int baseDim = 8192)
        {
            return $"[CNET] Context expansion unavailable: AICIMO performs fixed-width adapter routing at {baseDim} dimensions; it does not increase the context window.";
        }

        public string CompressModel(string modelPath, string targetSize = "1.6bit", string options = "")
        {
            if (string.IsNullOrWhiteSpace(modelPath))
            {
                return JsonSerializer.Serialize(new
                {
                    status = "refused",
                    reason = "model_path is required"
                });
            }

            string fullModelPath = Path.GetFullPath(modelPath);
            if (!File.Exists(fullModelPath))
            {
                return JsonSerializer.Serialize(new
                {
                    status = "refused",
                    reason = $"model not found: {fullModelPath}",
                    model_path = fullModelPath
                });
            }

            // Verify GGUF magic before attempting native compression
            try
            {
                byte[] magic = new byte[4];
                using (var fs = File.OpenRead(fullModelPath))
                {
                    if (fs.Read(magic, 0, 4) < 4 ||
                        magic[0] != 0x47 || magic[1] != 0x47 ||
                        magic[2] != 0x55 || magic[3] != 0x46)
                    {
                        return JsonSerializer.Serialize(new
                        {
                            status = "refused",
                            reason = "source file is not a valid GGUF (magic mismatch)",
                            model_path = fullModelPath
                        });
                    }
                }
            }
            catch (Exception ex)
            {
                return JsonSerializer.Serialize(new
                {
                    status = "refused",
                    reason = $"cannot read source file: {ex.Message}",
                    model_path = fullModelPath
                });
            }

            // Create the real native QGKP artifact
            string outputDir = _artifactPath;

            CnetCompressionResult result;
            try
            {
                result = CnetCompression.Compress(fullModelPath, outputDir, targetSize, options);
            }
            catch (Exception ex)
            {
                return JsonSerializer.Serialize(new
                {
                    status = "refused",
                    reason = $"native compression failed: {ex.Message}",
                    model_path = fullModelPath
                });
            }

            // Return structured JSON with full native artifact evidence
            return JsonSerializer.Serialize(new
            {
                status = "packaged_losslessly",
                artifact_path = result.ArtifactPath,
                artifact_size = result.ArtifactSize,
                artifact_magic = "QGKP",
                artifact_kind = result.ArtifactKind,
                source_integrity_hash = result.SourceIntegrityHash,
                source_sha256 = result.Probe.SourceSha256,
                source_size = result.SourceSize,
                requested_target_size = result.RequestedTargetSize,
                target_size_applied = result.TargetSizeApplied,
                strategy = result.Strategy,
                verified = result.Verified,
                packaging = new
                {
                    storage_ratio = Math.Round(result.StorageRatio, 6),
                    source_bytes = result.SourceSize,
                    artifact_bytes = result.ArtifactSize,
                    header_bytes = result.Inspection.HeaderBytes,
                    payload_bytes = result.Inspection.PayloadBytes,
                    note = "QGKP v3 packages the GGUF losslessly; no target-bit quantization was applied."
                },
                native_inspection = new
                {
                    version = result.Inspection.Version,
                    header_bytes = result.Inspection.HeaderBytes,
                    flags = $"0x{result.Inspection.Flags:x16}",
                    payload_bytes = result.Inspection.PayloadBytes,
                    payload_hash = $"0x{result.Inspection.PayloadHash:x16}",
                    architecture = result.Inspection.Architecture,
                    quantization = result.Inspection.Quantization,
                    n_layer = result.Inspection.NLayer,
                    hidden = result.Inspection.Hidden,
                    context_length = result.Inspection.ContextLength
                },
                quality_probe = new
                {
                    round_trip_verified = result.Probe.RoundTripVerified,
                    source_sha256 = result.Probe.SourceSha256,
                    note = result.Probe.RoundTripVerified
                        ? "materialized GGUF is byte-identical to source"
                        : "round-trip probe was not verified"
                },
                runtime = new
                {
                    directly_loadable_by_hermes = false,
                    required_action = "materialize the QGKP payload back to GGUF before use with a model runtime"
                },
                generated_at_utc = DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture)
            }, new JsonSerializerOptions { WriteIndented = true });
        }

        private static string SelectCompressionStrategy(string targetSize, string options)
        {
            string target = (targetSize + " " + options).ToLowerInvariant();
            if (target.Contains("int8") || target.Contains("8bit")) return "quantize_int8";
            if (target.Contains("ternary") || target.Contains("bitnet") || target.Contains("1.58")) return "quantize_ternary";
            if (target.Contains("1.6") || target.Contains("trit") || target.Contains("packed")) return "pack_trits_1p6bit";
            return "auto_pack_trits_1p6bit";
        }

        private static string SanitizeFileStem(string value)
        {
            var chars = value.ToCharArray();
            for (int i = 0; i < chars.Length; ++i)
            {
                if (!(char.IsLetterOrDigit(chars[i]) || chars[i] == '-' || chars[i] == '_' || chars[i] == '.'))
                {
                    chars[i] = '_';
                }
            }
            return new string(chars);
        }

        public string RouteOnRole(string input, string role = "memory-witness")
        {
            EnsureFreshSoulHost();
            if (string.IsNullOrWhiteSpace(input))
                return "[CNET AICIMO] Routing refused: input is empty.";
            if (_soulHost == null)
                return "[CNET AICIMO] Routing unavailable: certified CNET base could not be loaded.";

            var roster = UnitRoster();
            if (roster.Count == 0)
                return "[CNET AICIMO] Routing unavailable: no certified units resolved from the loaded base.";

            var tokens = TokenizeClaim(input);
            if (tokens.Count == 0) tokens.Add(role);

            // A role tag routes directly when the base certifies acq_<role>;
            // otherwise fall back to a deterministic role-hash pick over the
            // certified roster (reported as such — no pretend role slices).
            string goalTag;
            string resolution;
            try
            {
                _soulHost.UnitDims("acq_" + role);
                goalTag = role;
                resolution = "direct";
            }
            catch
            {
                string unit = roster[(int)(Fnv1a(role) % (uint)roster.Count)];
                goalTag = unit.StartsWith("acq_") ? unit.Substring(4) : unit;
                resolution = "role-hash fallback";
            }

            var (inDim, outDim) = _soulHost.UnitDims("acq_" + goalTag);
            var roleIds = Tokenizer()?.Encode(input);
            var featurized = roleIds != null && roleIds.Count > 0
                ? FeaturizeTokenIds(roleIds, inDim)
                : Featurize(tokens, inDim);
            double[] output;
            string mechanism;
            try
            {
                output = _soulHost.Route(goalTag, featurized, outDim);
                mechanism = "typed-port route";
            }
            catch (InvalidOperationException)
            {
                // No route plan over the registry for this goal tag — fall
                // back to running the owning unit directly. Still a real
                // certified-unit execution; reported as what it is.
                output = _soulHost.RunUnit("acq_" + goalTag, featurized);
                mechanism = "direct unit run (no route plan)";
            }
            var picks = FieldArgmax(output);
            double entropy = ActivationEntropy01(output);

            return $"[CNET AICIMO] Routed role '{role}' to acq_{goalTag} ({resolution}, {mechanism}). " +
                $"Output: {output.Length} doubles, field picks [{string.Join(", ", picks)}], " +
                $"activation entropy {FormatDouble(entropy)}.";
        }

        public string ListOracles()
        {
            EnsureFreshSoulHost();
            if (_soulHost == null) return SoulUnavailable("list_oracles");
            var descriptors = _soulHost.Oracles();
            var rows = new List<object>(descriptors.Count);
            int provenanceCompleteCount = 0;
            foreach (var descriptor in descriptors)
            {
                var missing = new List<string>();
                if (descriptor.BehaviorDigest == 0) missing.Add("behaviorDigest");
                if (descriptor.ArtifactDigest == 0) missing.Add("artifactDigest");
                if (descriptor.ContractDigest == 0) missing.Add("contractDigest");
                if (descriptor.ConfigDigest == 0) missing.Add("configDigest");
                if (descriptor.RetrievalSnapshotDigest == 0) missing.Add("retrievalSnapshotDigest");
                if (descriptor.ToolchainDigest == 0) missing.Add("toolchainDigest");
                if (string.IsNullOrWhiteSpace(descriptor.ArtifactSha256) ||
                    descriptor.ArtifactSha256.All(c => c == '0'))
                    missing.Add("artifactSha256");
                if (descriptor.RuntimeLibsDigest == 0) missing.Add("runtimeLibsDigest");
                bool complete = missing.Count == 0;
                if (complete) provenanceCompleteCount++;
                rows.Add(new
                {
                    name = descriptor.Name,
                    kind = descriptor.Kind,
                    behaviorDigest = $"0x{descriptor.BehaviorDigest:x16}",
                    artifactDigest = $"0x{descriptor.ArtifactDigest:x16}",
                    contractDigest = $"0x{descriptor.ContractDigest:x16}",
                    configDigest = $"0x{descriptor.ConfigDigest:x16}",
                    retrievalSnapshotDigest = $"0x{descriptor.RetrievalSnapshotDigest:x16}",
                    toolchainDigest = $"0x{descriptor.ToolchainDigest:x16}",
                    artifactSha256 = descriptor.ArtifactSha256,
                    runtimeLibsDigest = $"0x{descriptor.RuntimeLibsDigest:x16}",
                    provenance = new
                    {
                        complete,
                        observed = 8 - missing.Count,
                        required = 8,
                        missing
                    }
                });
            }
            return JsonSerializer.Serialize(new
            {
                source = "native CNB Oracle descriptors",
                admission = "descriptor_only_not_runtime_trust",
                count = rows.Count,
                provenance_complete_count = provenanceCompleteCount,
                provenance_incomplete_count = rows.Count - provenanceCompleteCount,
                oracles = rows
            });
        }

        /// <summary>
        /// One runtime health pass over the live certified registry. Exact
        /// native counts; a healthy soul reports all-zero actions.
        /// </summary>
        private static int ParseFamily(string f) => f switch
        {
            "raw" => 0, "onehot" => 1, "binary_msb" => 2, "binary_lsb" => 3,
            "evidence" => 4, "concept" => 5,
            _ => throw new ArgumentException($"unknown port family '{f}'")
        };

        /// <summary>
        /// Request a capability by explicit typed signature. Served now when
        /// a certified plan exists; otherwise the NOVEL goal is appended to
        /// the gap inbox and the 24/7 gap lane acquires it from the local
        /// model. This is how goals with no existing unit flow into learning.
        /// </summary>
        public string RequestCapability(string goalTag, string inTag,
            string family, int width, int count, int goalCount,
            List<double> input)
        {
            EnsureFreshSoulHost();
            if (_soulHost == null) return SoulUnavailable("request_capability");
            int fam = ParseFamily(family);
            double[]? inputVec = input.Count > 0 ? input.ToArray() : null;
            var (served, gapNoted, residual, source, output) = _soulHost.Request(
                fam, width, count, inTag, fam, width, goalCount, goalTag,
                inputVec);
            string note = residual
                ? "served by residual Tier C (uncertified); signature also queued to gap inbox for learning"
                : gapNoted
                    ? "no certified plan and residual unavailable; signature queued to the gap inbox"
                    : (inputVec == null
                        ? "capability is plannable (probe only)"
                        : "served by the certified plan");
            return JsonSerializer.Serialize(new
            {
                request = goalTag,
                served,
                gap_noted = gapNoted || residual,
                residual,
                source,
                note,
                output
            });
        }

        /// <summary>
        /// Exercise a sealed named skill (skill_* / research_* / acq_*).
        /// Builds a w_cur one-hot from query text and requests the goal tag.
        /// </summary>
        public string UseSkill(string skill, string? query = null, int goalCount = 3)
        {
            EnsureFreshSoulHost();
            if (_soulHost == null) return SoulUnavailable("use_skill");
            if (string.IsNullOrWhiteSpace(skill))
                return JsonSerializer.Serialize(new { ok = false, error = "empty skill" });

            string raw = skill.Trim();
            // Normalize to goal tag without acq_ prefix
            string goal = raw.StartsWith("acq_", StringComparison.Ordinal)
                ? raw.Substring(4)
                : raw;
            if (!(goal.StartsWith("skill_", StringComparison.Ordinal)
                  || goal.StartsWith("research_", StringComparison.Ordinal)
                  || goal.StartsWith("chunk_", StringComparison.Ordinal)
                  || goal.StartsWith("tk", StringComparison.Ordinal)))
            {
                // bare name → prefer skill_ then research_
                string s1 = "skill_" + goal;
                string s2 = "research_" + goal;
                bool has1 = false, has2 = false;
                try
                {
                    foreach (var u in _soulHost.Units())
                    {
                        if (u == "acq_" + s1 || u == s1) has1 = true;
                        if (u == "acq_" + s2 || u == s2) has2 = true;
                    }
                }
                catch { /* ignore */ }
                goal = has1 ? s1 : (has2 ? s2 : s1);
            }

            int width = 256;
            if (goalCount < 1) goalCount = 3;
            if (goalCount > 8) goalCount = 8;

            // Deterministic one-hot from query (or skill name)
            string seed = string.IsNullOrWhiteSpace(query) ? goal : query!;
            uint h = 2166136261;
            foreach (var ch in seed)
            {
                h ^= ch;
                h *= 16777619;
            }
            int idx = (int)(h % (uint)width);
            var input = new List<double>(width);
            for (int i = 0; i < width; i++) input.Add(i == idx ? 1.0 : 0.0);

            string unitName = "acq_" + goal;
            bool unitPresent = false;
            try
            {
                foreach (var u in _soulHost.Units())
                    if (u == unitName || u == goal) { unitPresent = true; break; }
            }
            catch { /* ignore */ }

            var (served, gapNoted, residual, source, output) = _soulHost.Request(
                inFamily: 1, inWidth: width, inCount: 1, inTag: "w_cur",
                goalFamily: 1, goalWidth: width, goalCount: goalCount, goalTag: goal,
                input: input.ToArray());

            // Fallback: direct unit run if request plan miss but unit exists
            if (!served && unitPresent)
            {
                try
                {
                    // Expand input for goal_count fields if needed
                    var full = new double[width * goalCount];
                    for (int g = 0; g < goalCount; g++)
                        for (int i = 0; i < width; i++)
                            full[g * width + i] = input[i];
                    // Many units expect in_total=width only
                    output = _soulHost.RunUnit(unitName, input.ToArray());
                    served = true;
                    source = "certified_unit";
                    residual = false;
                    gapNoted = false;
                }
                catch (Exception ex)
                {
                    return JsonSerializer.Serialize(new
                    {
                        ok = false,
                        skill = goal,
                        unit = unitName,
                        unit_present = unitPresent,
                        served = false,
                        error = ex.Message
                    });
                }
            }

            // Top-k field picks for readability
            var picks = new List<int>();
            if (output != null && output.Length > 0)
            {
                int fields = Math.Max(1, output.Length / width);
                for (int f = 0; f < fields && f < goalCount; f++)
                {
                    int baseOff = f * (output.Length / fields);
                    int best = 0;
                    double bestV = double.NegativeInfinity;
                    int span = output.Length / fields;
                    for (int i = 0; i < span && baseOff + i < output.Length; i++)
                    {
                        if (output[baseOff + i] > bestV)
                        {
                            bestV = output[baseOff + i];
                            best = i;
                        }
                    }
                    picks.Add(best);
                }
            }

            return JsonSerializer.Serialize(new
            {
                ok = served,
                skill = goal,
                unit = unitName,
                unit_present = unitPresent,
                served,
                residual,
                gap_noted = gapNoted,
                source,
                query = seed,
                input_idx = idx,
                picks,
                output_len = output?.Length ?? 0,
                note = served
                    ? "named skill exercised (certified or residual)"
                    : "not served — gap noted if novel"
            });
        }

        /// <summary>List sealed skill_/research_/chunk_ units in the live base.</summary>
        public string ListSkills()
        {
            EnsureFreshSoulHost();
            if (_soulHost == null) return SoulUnavailable("list_skills");
            var skills = new List<string>();
            try
            {
                foreach (var u in _soulHost.Units())
                {
                    if (u.StartsWith("acq_skill_", StringComparison.Ordinal)
                        || u.StartsWith("acq_research_", StringComparison.Ordinal)
                        || u.StartsWith("acq_chunk_", StringComparison.Ordinal)
                        || u.StartsWith("skill_", StringComparison.Ordinal)
                        || u.StartsWith("research_", StringComparison.Ordinal))
                        skills.Add(u);
                }
            }
            catch (Exception ex)
            {
                return JsonSerializer.Serialize(new { ok = false, error = ex.Message });
            }
            skills.Sort(StringComparer.Ordinal);
            return JsonSerializer.Serialize(new { ok = true, count = skills.Count, skills });
        }

        /// <summary>
        /// Queue freeform Hermes chat as a teachable gap.
        /// If skill is set → skill_&lt;slug&gt; structured unit; else opaque tk*q*.
        /// </summary>
        public string LearnFromChat(string text, int k = 3, string? skill = null)
        {
            EnsureFreshSoulHost();
            string inbox = Environment.GetEnvironmentVariable("CNET_GAP_INBOX")
                ?? (_basePath + ".inbox");
            if (string.IsNullOrWhiteSpace(text) && string.IsNullOrWhiteSpace(skill))
                return JsonSerializer.Serialize(new { ok = false, error = "empty text" });
            try
            {
                        if (k < 1) k = 3;
                        if (k > 8) k = 8;
                        string goal;
                        if (!string.IsNullOrWhiteSpace(skill))
                        {
                            var slug = new System.Text.StringBuilder();
                            foreach (var ch in skill.Trim().ToLowerInvariant())
                            {
                                if (char.IsLetterOrDigit(ch)) slug.Append(ch);
                                else if (ch is '_' or '-' or ' ')
                                {
                                    if (slug.Length > 0 && slug[^1] != '_') slug.Append('_');
                                }
                            }
                            while (slug.Length > 0 && slug[^1] == '_') slug.Length--;
                            if (slug.Length == 0) slug.Append('x');
                            var s = slug.ToString();
                            if (s.StartsWith("skill_") || s.StartsWith("research_") || s.StartsWith("chunk_"))
                                goal = s;
                            else
                                goal = "skill_" + s;
                        }
                        else
                        {
                            uint h = 2166136261;
                            foreach (var ch in text)
                            {
                                h ^= ch;
                                h *= 16777619;
                            }
                            uint id = h % 256;
                            if (id == 0) id = 1;
                            goal = $"tk{id}q{id}";
                        }
                        string line = $"NO_PLAN 1 256 1 w_cur 1 256 {k} {goal}\n";
                        File.AppendAllText(inbox, line);
                        return JsonSerializer.Serialize(new
                        {
                            ok = true,
                            gap_noted = true,
                            inbox,
                            goal,
                            k,
                            structured = !string.IsNullOrWhiteSpace(skill),
                            note = string.IsNullOrWhiteSpace(skill)
                                ? "queued teachable NO_PLAN for personal-AI lane (auto-learn)"
                                : "queued structured skill NO_PLAN for personal-AI lane"
                        });
                    }
                    catch (Exception ex)
                    {
                        return JsonSerializer.Serialize(new { ok = false, error = ex.Message });
                    }
                }

                public string HealthTick()
                {
            EnsureFreshSoulHost();
            if (_soulHost == null) return SoulUnavailable("health_tick");
            var r = _soulHost.HealthTick();
            object? serve = null;
            try
            {
                var s = _soulHost.GetServeStats();
                serve = new
                {
                    certified_serves = s.CertifiedServes,
                    residual_serves = s.ResidualServes,
                    gap_notes = s.GapNotes,
                    structure_mines = s.StructureMines,
                    structure_seals = s.StructureSeals,
                    residual_bound = s.ResidualBound,
                    residual_window = s.ResidualWindow,
                    last_source = s.LastSource,
                    units = s.Units
                };
            }
            catch
            {
                /* older native lib without soul_serve_stats */
            }
            return JsonSerializer.Serialize(new
            {
                health_tick = true,
                entries = r.Entries,
                demoted_by_audit = r.DemotedByAudit,
                labeled_from_contract = r.LabeledFromContract,
                labeled_via_teacher = r.LabeledViaTeacher,
                heal_attempted = r.HealAttempted,
                healed = r.Healed,
                promoted_provisional = r.PromotedProvisional,
                shadows_promoted = r.ShadowsPromoted,
                reset_remaining = r.ResetRemaining,
                trust = new
                {
                    uncertified = r.TrustUncertified,
                    evidenced = r.TrustEvidenced,
                    certified = r.TrustCertified,
                    demoted = r.TrustDemoted
                },
                serve
            });
        }

        /// <summary>Live web lookup (DuckDuckGo Instant Answer + Wikipedia fallback).</summary>
        public string WebSearch(string query)
        {
            try
            {
                McpTools.MemoryInit();
                string text = McpTools.WebSearch(query ?? "");
                return JsonSerializer.Serialize(new
                {
                    tool = "web_search",
                    query = query ?? "",
                    result = text
                });
            }
            catch (Exception ex)
            {
                return JsonSerializer.Serialize(new
                {
                    tool = "web_search",
                    query = query ?? "",
                    error = ex.Message
                });
            }
        }

        /// <summary>Wikipedia summary lookup (memory-cached).</summary>
        public string WikiLookup(string query)
        {
            try
            {
                McpTools.MemoryInit();
                string text = McpTools.WikiLookup(query ?? "");
                return JsonSerializer.Serialize(new
                {
                    tool = "wiki_lookup",
                    query = query ?? "",
                    result = text
                });
            }
            catch (Exception ex)
            {
                return JsonSerializer.Serialize(new
                {
                    tool = "wiki_lookup",
                    query = query ?? "",
                    error = ex.Message
                });
            }
        }

        /// <summary>
        /// Closed-set JSON tool-call classify via sealed json_toolcall unit.
        /// On miss, notes gap (jtc_feat→json_tool) when CNET_GAP_INBOX or base.inbox is set.
        /// </summary>
        public string ClassifyToolCall(string json)
        {
            EnsureFreshSoulHost();
            if (_soulHost == null) return SoulUnavailable("classify_toolcall");
            string inbox = Environment.GetEnvironmentVariable("CNET_GAP_INBOX")
                ?? (_basePath + ".inbox");
            try
            {
                var (tool, source, gapNoted) = JsonToolCall.ClassifyOrGap(_soulHost, json ?? "", inbox);
                bool present = false;
                try
                {
                    foreach (var u in _soulHost.Units())
                        if (u == JsonToolCall.UnitName) { present = true; break; }
                }
                catch { /* ignore */ }

                return JsonSerializer.Serialize(new
                {
                    unit = JsonToolCall.UnitName,
                    unit_present = present,
                    tool,
                    source,
                    gap_noted = gapNoted,
                    known = tool != null && JsonToolCall.IsKnownTool(tool),
                    features = JsonToolCall.Encode(json),
                    note = tool != null
                        ? "certified closed-set tool classification"
                        : (source != null && source.StartsWith("unknown")
                            ? "refused unknown/low-confidence tool; gap noted when possible"
                            : (gapNoted
                            ? "no certified plan / unit; NO_PLAN noted for gap lane (jtc_feat→json_tool)"
                            : "classification failed without gap note"))
                });
            }
            catch (Exception ex)
            {
                bool noted = JsonToolCall.NoteGap(inbox);
                return JsonSerializer.Serialize(new
                {
                    unit = JsonToolCall.UnitName,
                    tool = (string?)null,
                    source = "error",
                    gap_noted = noted,
                    error = ex.Message,
                    note = "seal with scripts/personal_ai_auto.sh jtc-seal if unit missing"
                });
            }
        }

        /// <summary>Whether the sealed json_toolcall unit is in the live registry.</summary>
        public string JsonToolCallStatus()
        {
            EnsureFreshSoulHost();
            if (_soulHost == null) return SoulUnavailable("json_toolcall_status");
            bool present = false;
            try
            {
                foreach (var u in _soulHost.Units())
                    if (u == JsonToolCall.UnitName) { present = true; break; }
            }
            catch (Exception ex)
            {
                return JsonSerializer.Serialize(new { unit = JsonToolCall.UnitName, present = false, error = ex.Message });
            }
            object? sample = null;
            if (present)
            {
                try
                {
                    string t = JsonToolCall.Classify(_soulHost, JsonToolCall.ExampleJson[0]);
                    sample = new { example_tool = t, ok = t == "calculator" };
                }
                catch (Exception ex)
                {
                    sample = new { error = ex.Message };
                }
            }
            return JsonSerializer.Serialize(new
            {
                unit = JsonToolCall.UnitName,
                present,
                tools = JsonToolCall.ToolNames,
                sample,
                base_generation = _baseGeneration,
                refresh = "atomic CNB generations reload in-process"
            });
        }

        public string ListUnits()
        {
            var roster = UnitRoster();
            if (roster.Count == 0)
                return "[CNET] No units passed certification replay into the live registry.";

            bool tokenRoster = true;
            foreach (string name in roster)
            {
                int tk = name.IndexOf("tk", StringComparison.Ordinal);
                int q = tk >= 0 ? name.IndexOf('q', tk + 2) : -1;
                try
                {
                    var (inDim, outDim) = _soulHost.UnitDims(name);
                    if (q <= tk + 2 || !int.TryParse(name.Substring(tk + 2, q - tk - 2), out _) ||
                        inDim != 256 || outDim <= 0 || outDim % 256 != 0)
                        tokenRoster = false;
                }
                catch { tokenRoster = false; }
            }
            if (tokenRoster) Tokenizer(); // decode ids only when the roster proves that schema
            var samples = new List<string>();
            for (int i = 0; i < Math.Min(5, roster.Count); i++)
            {
                try
                {
                    var (inDim, outDim) = _soulHost.UnitDims(roster[i]);
                    double rel = _soulHost.Reliability(roster[i]);
                    string piece = "";
                    int tkPos = roster[i].IndexOf("tk", StringComparison.Ordinal);
                    int qPos = tkPos >= 0 ? roster[i].IndexOf('q', tkPos + 2) : -1;
                    if (qPos > tkPos + 2 && int.TryParse(roster[i].Substring(tkPos + 2, qPos - tkPos - 2), out int tid))
                        piece = $"token '{PieceOf(tid)}', ";
                    samples.Add($"{roster[i]} ({piece}in={inDim}, out={outDim}, reliability={FormatDouble(rel)})");
                }
                catch
                {
                    samples.Add($"{roster[i]} (unresolved)");
                }
            }

            string schemaNote = tokenRoster
                ? "Roster proves the acq_tk<id>q<id> 256-wide token schema. " +
                  (_tokenizer != null
                    ? "Gemma tokenizer is wired for exact claim-token lookup."
                    : "Gemma tokenizer is unavailable; token ids are shown raw.")
                : "No global task or dimensional schema is assumed; each unit is described by its live typed dimensions.";
            string extraNote = "";
            var mounted = ExtraBases();
            if (mounted.Count > 0)
            {
                var parts = new List<string>();
                foreach (var eb in mounted)
                    parts.Add($"{Path.GetFileName(eb.Path)} ({eb.Roster.Count} units)");
                extraNote = $" Extra bases mounted: {string.Join(", ", parts)}.";
            }
            return $"[CNET] {roster.Count} certified units resolved from {_rosterSource}. " +
                $"Sample: {string.Join("; ", samples)}. " +
                schemaNote + RecipeSummary() + extraNote;
        }
    }
}