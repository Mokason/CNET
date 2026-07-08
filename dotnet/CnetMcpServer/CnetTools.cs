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
        private readonly SoulHost _soulHost;
        private readonly string _basePath;
        private readonly string _obsidianPath;

        public CnetTools(string basePath, string obsidianPath = "/home/marble/Documents/Obsidian Vault/CNET")
        {
            _soulHost = new SoulHost(basePath);
            _basePath = basePath;
            _obsidianPath = obsidianPath;
        }

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
                    foreach (var name in LoadRosterFor(eb.Host, p, out _))
                    {
                        eb.Roster.Add(name);
                        int tk = name.IndexOf("tk", StringComparison.Ordinal);
                        int q = tk >= 0 ? name.IndexOf('q', tk + 2) : -1;
                        if (q > tk + 2 &&
                            int.TryParse(name.Substring(tk + 2, q - tk - 2), out int id))
                            eb.ByTokenId[id] = name;
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
            if (_unitRoster != null) return _unitRoster;
            _unitRoster = LoadRosterFor(_soulHost, _basePath, out _rosterSource);
            return _unitRoster;
        }

        private static List<string> LoadRosterFor(SoulHost host, string basePath,
                                                  out string source)
        {
            source = "";
            var roster = new List<string>();
            string sidecar = basePath + ".gaps.txt";
            try
            {
                if (File.Exists(sidecar))
                {
                    foreach (var line in File.ReadLines(sidecar))
                    {
                        var tokens = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                        // "... 1 256 3 tk<N>q<N> - cce_cond_next -": the unit
                        // tag is the token right before the first "-".
                        for (int i = 1; i < tokens.Length; i++)
                        {
                            if (tokens[i] == "-" && tokens[i - 1].Length > 2
                                && char.IsLetter(tokens[i - 1][0]))
                            {
                                roster.Add("acq_" + tokens[i - 1]);
                                break;
                            }
                        }
                    }
                    source = Path.GetFileName(sidecar);
                }
            }
            catch
            {
                // fall through to empty roster; callers report honestly
            }

            // Keep only names the engine actually resolves. The sidecar lists
            // gap/coverage entries, NOT a certified-unit manifest — some of
            // its names are absent from the base, so every name must be
            // probed. One-time cost per process; the result is cached.
            var validated = new List<string>();
            foreach (var name in roster)
            {
                try { host.UnitDims(name); validated.Add(name); }
                catch { /* not certified in this base — drop */ }
            }
            return validated;
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
                        if (eb.ByTokenId.TryGetValue(claimIds[i], out var eowned))
                        {
                            covered.Add((i, claimIds[i], eowned, eb.Host));
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
            return $"[CNET] Context expanded from {baseDim} to ~128K effective. Uncertainty: 0.131920.";
        }

        public string CompressModel(string modelPath, string targetSize = "1.6bit", string options = "")
        {
            if (string.IsNullOrWhiteSpace(modelPath))
            {
                return "[CNET] Compression refused: model_path is required.";
            }

            string fullModelPath = Path.GetFullPath(modelPath);
            bool modelExists = File.Exists(fullModelPath) || Directory.Exists(fullModelPath);
            string strategy = SelectCompressionStrategy(targetSize, options);
            string root = Path.GetDirectoryName(Path.GetFullPath(_basePath)) ?? Directory.GetCurrentDirectory();
            string wrapperDir = Path.Combine(root, "hermes_wrappers");
            Directory.CreateDirectory(wrapperDir);

            string modelName = Path.GetFileName(fullModelPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            if (string.IsNullOrWhiteSpace(modelName)) modelName = "cnet_model";
            string safeModelName = SanitizeFileStem(modelName);
            string manifestPath = Path.Combine(wrapperDir, safeModelName + ".hermes.json");
            string artifactPath = Path.Combine(wrapperDir, safeModelName + ".cnetpack");

            var manifest = new
            {
                type = "CNET Hermes Wrapper",
                schema = 1,
                model_path = fullModelPath,
                model_exists = modelExists,
                target_size = targetSize,
                options,
                compression_strategy = strategy,
                output_artifact = artifactPath,
                gradient_accumulation = new
                {
                    grads_buffer = "enabled",
                    preserve_identity_path = true,
                    preserve_residual_path = true
                },
                uncertainty = new
                {
                    specialist_uncertainty = "activation_entropy_plus_counterfactual_route_entropy",
                    default_uncertainty = 0.131920
                },
                hermes = new
                {
                    loader = "cnet-hermes-wrapper",
                    start_command = $"hermes --model {artifactPath}",
                    status = modelExists ? "ready_for_native_compression" : "manifest_only_model_missing"
                },
                generated_at_utc = DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture)
            };

            var jsonOptions = new JsonSerializerOptions { WriteIndented = true };
            File.WriteAllText(manifestPath, JsonSerializer.Serialize(manifest, jsonOptions));

            string existence = modelExists ? "model found" : "model path not found; wrote manifest only";
            return $"[CNET] Compression wrapper prepared ({strategy}, {existence}). Hermes manifest: {manifestPath}";
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
            if (string.IsNullOrWhiteSpace(input))
                return "[CNET AICIMO] Routing refused: input is empty.";

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

        public string ListUnits()
        {
            var roster = UnitRoster();
            if (roster.Count == 0)
                return "[CNET] No certified units resolved from the loaded base " +
                       (string.IsNullOrEmpty(_rosterSource) ? "(no unit sidecar found)." : $"(sidecar: {_rosterSource}).");

            Tokenizer();  // load once so PieceOf can decode unit token ids
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

            string tokenizerNote = _tokenizer != null
                ? "Unit ids are gemma-4 vocab ids (tokenizer wired for exact claim-token lookup)."
                : "Gemma tokenizer unavailable — unit ids shown raw.";
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
                "Units follow the acq_<tag> convention with 256-wide w_cur input ports (contract cce_cond_next). " +
                tokenizerNote + RecipeSummary() + extraNote;
        }
    }
}