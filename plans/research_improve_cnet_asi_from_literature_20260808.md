# Research brief: improve CNET ASI from recent literature (2026-08-08)

**Scope:** What peer literature suggests we can do to improve **CNET-style ASI**  
(specialists + CERT + fail-closed coverage + teacher/runtime split + skill/capsule libraries).  
**Not** a bid for AGI. Unit count alone is not intelligence.

**CNET inventory at research time (disk):** ~399 CERTs in `live_eight_work.cnb` (mostly `acq_tk*`); ~53 portable `skill_pos_*`; ~O(6–10) domain packs; director learned promote WITHHELD.

**Law unchanged:** never lower floors; no Tier-A self-train; teacher ≠ runtime; abstain first-class.

---

## Executive map (priority × fit)

| Priority | Theme | CNET lever | Literature anchors |
|---:|---|---|---|
| P0 | Skill library ops + retrieval at scale | catalog, contracts, gate executability | SkillCenter, SkillOps, SkillZip, SkillTrace, Don't Offer…, capability pages |
| P0 | Principled abstain / defer | coverage + conformal + multi-expert defer | Conformal abstention, L2D multi-expert, OOD survey, AdaMoE null expert |
| P1 | Routing specialization without collapse | sparse rank / ROUTE / center | DeepSeekMoE, representation collapse, Expert Choice, elbow/max-score routing, SpecDrop |
| P1 | Composition graphs not bags of skills | ports + LINK + procedure chunks | SkillTrace, SkillZip contracts, neurosymbolic CompGen, AdapterFusion |
| P1 | Continual expand without wrecking old CERT | FORM/STM/LTM + progressive admit | Progressive NN, PackNet, dynamic expansion, ContinualSkillBench |
| P2 | Episodic memory as first-class | mem_runtime beyond resolve | Episodic memory position paper |
| P2 | Multi-role enforcement | constellation packs | TeamBench role separation |
| P2 | Cheaper certified eval | gate design | AV-AIVAT anytime-valid stopping |
| P2 | Robot path | affordance gating + atomic skills | SayCan, atomic skill library, DyPES-VLA |
| P3 | Tool privilege + guardrails | capsule masks | over-privileged tools, Proof-of-Guardrail, NFL guardrails |
| avoid | Soft MoE / dense always-on experts as core | conflicts with fail-closed sparse CERT | Soft MoE useful only as optional continuous sidecar research |

---

## 1. Skill libraries (biggest product-shaped upside)

### What literature is doing
- **Voyager:** open-ended agent = curriculum + **ever-growing executable skill library** + iterative prompting — skills as code, not weights.[11]
- **SkillCenter:** source-grounded library at **~2e5** skills with gated ingestion pipelines (scale of *catalog*, not one monobrain).[9]
- **SKILLFOUNDRY:** self-evolving libraries from heterogeneous scientific sources.[10]
- **SkillOps:** treats libraries as **software ecosystems** with *skill technical debt* (retrieval/composition rot across the catalog).[23]
- **SkillZip:** compress skill graphs under context budget while **preserving procedural contracts**.[24]
- **SkillTrace:** compose via **query–skill graph**, not top-1 semantic hit.[29]
- **Capability pages / neighbors:** retrieval fails when docs don't say *what nearby skills are for* — cluster-contrastive capability pages.[30]
- **Don't Offer What Can't Be Done:** production three-stage pipeline — semantic recall → **deterministic executability gate** → final pick.[28]
- **Atomic skills (embodied):** decompose end-to-end policies into atomic library for data-efficient manipulation.[21]
- **Self-improving skill RL:** skill library + RL for consistent improvement beyond pure prompting.[22]
- **ContinualSkillBench:** ask whether agents *actually* evolve capabilities via skill libraries (eval discipline).[47]
- **Skill-as-pseudocode:** refactor skills into structured pseudocode for cleaner composition.[42]

### What to do in CNET
1. **SKU law for every portable unit**  
   Manifest fields beyond digest: `capability_page` (positive + **negative neighbors**), ports, preconditions, postconditions, privilege tier, teacher provenance. Aligns with [30][28][23].

2. **Three-stage resolve (mem_runtime upgrade)**  
   ```
   semantic/hash recall (STM→LTM)
     → deterministic executability/coverage gate (masks, ports, world state)
       → CERT step or ABSTAIN
   ```
   Direct map of [28]. Today coverage admits; **world-state executability** is the missing middle (quests, inventory, robot joints, tool auth).

3. **Composition = graph, not bag**  
   Promote procedure chunks + SkillTrace-style edges (depends_on, refinements, alternatives) into LTM index.[29][24]  
   Keep typed ports; SkillZip-like **contract-preserving** subset load when packing for host/Unity/robot.

4. **Library ops / anti-debt**  
   Janitor metrics from SkillOps: orphan skills, broken links, stale teachers, near-duplicate tags (you already refuse Levenshtein-1 tags — extend to capability-page collision).[23]

5. **Scale path honesty**  
   Literature “hundreds of thousands of skills” is **catalog scale** after filtering.[9] Your gap-lane `acq_tk*` is *not* that; grow **named atomic + domain packs**, and keep tk shards as teacher exhaust, not SKU count vanity.

6. **Self-improve only via FORM + external score**  
   RL-on-skills [22] is OK **only** as teacher/proposer off the CERT path; promote still needs holdout margin (your WITHHELD law). ContinualSkillBench [47] is the right gate family before claiming “library evolution works.”

---

## 2. Abstention, conformal risk, multi-expert defer

### Literature
- Conformal abstention for LLM hallucinations (self-eval similarity + CP).[13]
- Geometry-calibrated conformal abstention (post-hoc CA).[14]
- Cost-sensitive conformal + human-in-the-loop abstention for high-stakes imbalance.[20]
- Learning to defer with **multiple experts** + consistent surrogates.[2][41]
- AdaMoE **null experts** = adaptive “use nobody / abstain compute”.[5]
- OOD detection survey (generalized taxonomy).[31]

### CNET actions
1. **Promote conformal residual ring from Brain signal pack → main CNET admit/serve**  
   Holdout conformal quantile per unit; serve abstains if residual > q̂ — literature CP is the theory under your already-present conformal knobs.[13][14]

2. **Cost-sensitive abstain**  
   Different packs different false-serve cost (robot torque ≫ game dialogue). Encode cost in verify floors / serve gate, not one global MSE.[20]

3. **Learning-to-defer meta pack**  
   Meta does not CERT answers; it chooses `{unit_i | ABSTAIN | TEACHER}`. Multi-expert L2D is the math for “gardener routes among CERTs”.[2]

4. **Null-expert / empty admit as first-class route outcome**  
   Like AdaMoE null experts: unknown multiprobe → abstain without scanning all CERT (spine law already; keep it).[5]

5. **OOD taxonomy in gates**  
   Report semantic / covariate / domain-shift OOD separately per [31]; don't mix “missing route” with “coverage fail.”

---

## 3. Routing & specialist quality (avoid MoE theater)

### Literature
- Sparse MoE can **collapse representations** toward centroids — routing looks busy, experts not specialized.[3]
- **DeepSeekMoE:** fine-grained experts + **shared experts** for common knowledge vs specialized.[17]
- **Expert Choice:** experts pick tokens (load balance / specialization).[18]
- Switch Transformers: large sparse scaling baseline.[35]
- Max-score routing; elbow-based **training-free** dynamic k; SpecDrop category-conditioned **parameter-free** routing.[8][48][43]
- Soft MoE densifies routing (good research, weaker fail-closed story).[12]
- ModuleFormer / BTX: modularity from MoE; branch-train then mix experts.[16][1]
- SpecDrop warns: learned routers can lose to fixed category routing when signal granularity mismatches categories.[43]

### CNET actions
1. **Measure specialization, not just admit count**  
   Track per-unit coverage exclusivity, cross-unit interference (you have knowledge_accumulation_bench) + collapse diagnostics inspired by [3].

2. **Shared vs specialized split**  
   DeepSeek-style: a few **shared** residual/backbone maps (Brain continuous / ACCUM residual) + many **specialized** CNU1 leaves.[17] You already have LINK_ACCUM + pieces sidecar — name it and gate it.

3. **Expert-choice / capacity caps on hot skills**  
   Prevent popular ROUTE buckets from starving rare CERTs (load). Expert Choice inverted: **capacity per unit** in STM pin set.[18]

4. **Dynamic “how many specialists”**  
   Elbow/max-score at serve: variable top-M already in sparse board; add score-gap stop so you don't always fire M.[48][8]

5. **Category-conditioned routes (SpecDrop lesson)**  
   For discrete product domains (Director vs Steward vs tool family), **hard category prior** before learned/center rank can beat soft routers.[43] Matches multi-capsule separation.

6. **Branch-Train-Merge for packs**  
   Train domain packs in isolation (embarrassingly parallel), mix only at compose/host — BTX spirit without merging weights into one LM.[1]

7. **Do not soft-MoE the CERT core**  
   Soft mixing fights abstain and content-id law. Keep soft paths in continuous sidecar only.[12]

---

## 4. Continual learning & non-destructive expand

### Literature
- Progressive Neural Networks: lateral columns, freeze old.[19]
- PackNet: iterative prune → allocate capacity to new tasks.[4]
- Self-controlled dynamic expansion.[7]
- AdapterFusion: extract adapters then non-destructive fuse.[34]
- Mixture of LoRA Experts / hard-routed MoE-LoRA: select don't relearn.[36]
- Position: modular memory key to continual agents (from search set).
- ContinualSkillBench for skill evolution claims.[47]

### CNET actions
1. **CERT is PackNet/Progressive by construction** — new content-id, old frozen. Double down: never in-place mutate live CERT bytes.[4][19]

2. **FORM expand controller**  
   Self-controlled expansion [7]: spawn candidate only when residual/coverage pressure high; else refuse spawn (your recall-before-spawn).

3. **AdapterFusion-like compose for PEFT lane**  
   If MTK/LoRA stays secondary: fuse via gated composition, not sequential overwrite.[34][36] Keep MTK ≠ CERT law.

4. **Gate “library got smarter” with ContinualSkillBench-style suites**  
   Holdout tasks that only new skills should unlock; old tasks must not regress.[47]

---

## 5. Memory

### Literature
- Episodic memory as missing piece for long-term agents (single-shot instance contexts).[46]

### CNET actions
1. Extend mem_runtime: **episode records** (state, skill_id, receipt, outcome) separate from skill LTM.  
2. Retrieve episodes for FORM proposals / taint evidence — not as uncertified serve path.  
3. STM pins = hot CERTs; episodic = what *happened*; LTM = sealed skills. Three-way split matches paper + your lanes.

---

## 6. Composition, neurosymbolic world structure

### Literature
- Neurosymbolic compositional world models (CompGen on known atoms).[38]
- DFA-RAG: finite-automaton semantic router for conversational structure.[6]
- SkillTrace composition graphs.[29]

### CNET actions
1. Treat capsules as **atoms**; host state machine / DFA for dialogue and quest legality (you already do masks — push further).[6][45]  
2. CompGen tests: holdout **compositions** of known skills, not only atomic holdout.[38]  
3. Keep narrative free-text out of CERT core; boards + ids.

---

## 7. Tools, privileges, guardrails

### Literature
- Over-privileged tool selection is common; lower privilege often suffices.[26]
- Proof-of-Guardrail: what proofs of guards actually mean (and don't).[27]
- No free lunch with guardrails (tradeoffs).[32]
- Tool-to-agent retrieval at multi-agent scale.[25]
- Bitter lesson of tool calling / tools-as-code evaluation.[40]

### CNET actions
1. **Privilege tier on every tool capsule**; resolve prefers minimal privilege that executability allows.[26][28]  
2. Guardrails as **separate CERT monitors**, not vibes; don't trust a badge without adversarial eval.[27][32]  
3. json_toolcall family: keep sealed alphabets; evaluate programmatic chaining carefully [40] without replacing CERT with free code exec as authority.

---

## 8. Multi-agent / constellation evaluation

### Literature
- TeamBench: **enforced** role separation so one agent can't silently do another's job.[33]
- AV-AIVAT: anytime-valid certified stopping → far cheaper comparative agent eval.[39]

### CNET actions
1. Multi-capsule stress already exists — add **capability firewall tests** (Cast must not commit NarrativeLog, etc.) in TeamBench spirit.[33]  
2. Promotion battles (oracle vs learned head): anytime-valid stopping to cut eval cost without invalid optional stopping.[39]

---

## 9. Robotics path (if/when)

### Literature
- SayCan: language proposes, **affordances** ground what can run.[45]
- Atomic skill libraries for embodied manipulation.[21]
- Cross-embodiment: shared dynamics priors + embodiment-specific control.[44]

### CNET actions
1. RobotSkill schema = SayCan loop with CERT skills + safety commit host.[45]  
2. Atomic library grain for manipulation [21].  
3. Shared continuous dynamics sidecar + per-body CERT packs [44] — same Brain continuous / CNET discrete split.

---

## Concrete 90-day improvement backlog (law-safe)

### Slice A — Library OS (P0)
- [ ] `capability_page` + negative neighbors on capsule manifest  
- [ ] executability gate in resolve (state masks)  
- [ ] skill-graph edges in LTM (compose/alternative/refine)  
- [ ] SkillOps janitor: debt metrics + near-dup capability detection  
- [ ] Gate: retrieval@k on held-out tasks with executability hard-fail  

### Slice B — Abstain math (P0)
- [ ] Per-unit conformal quantile on holdout residuals at admit  
- [ ] Cost-sensitive serve thresholds by pack class  
- [ ] Meta L2D choose among top-M CERTs or abstain (no answer CERT)  
- [ ] Gate: coverage_abstain + calibrated FCR-style report  

### Slice C — Routing quality (P1)
- [ ] Specialization/collapse diagnostics on live CNB  
- [ ] Shared residual expert(s) + specialized leaves (document DeepSeek-like split)  
- [ ] Score-gap / elbow stop on top-M  
- [ ] Category prior before center rank for constellation packs  

### Slice D — Continual + memory (P1)
- [ ] Episodic store beside STM/LTM/FORM  
- [ ] ContinualSkillBench-like regression pack for old CERTs after FORM promote  
- [ ] Expand only under residual pressure (self-controlled)  

### Slice E — Eval economics (P2)
- [ ] Anytime-valid stopping for director head bake-offs  
- [ ] TeamBench-style role firewall suite for multi-capsule  

### Explicit non-goals
- Soft-MoE core, CE parrot residual fill, floor lowering, field self-CERT, equating MTK with capsules.

---

## Reading list (curated, high leverage first)

1. Don't Offer What Can't Be Done — executability gating [28]  
2. SkillOps — library debt [23]  
3. SkillZip / SkillTrace / capability pages — scale compose+retrieve [24][29][30]  
4. SkillCenter / Voyager — library north stars [9][11]  
5. Conformal abstention (+ geometry CA) [13][14]  
6. L2D multi-expert [2]  
7. DeepSeekMoE + MoE collapse [17][3]  
8. AdaMoE null experts [5]  
9. Expert Choice / elbow routing [18][48]  
10. SayCan + atomic skills [45][21]  
11. Progressive NN / PackNet [19][4]  
12. Episodic memory position [46]  
13. TeamBench + AV-AIVAT [33][39]  
14. ContinualSkillBench [47]  
15. SpecDrop (when not to trust learned routers) [43]  

Full arXiv abs links live in the Sources block below (ledger-backed).

---

## Sources

(Generated from retrieval ledger 2026-08-08; ids stable for this brief.)
## Sources

[1] https://arxiv.org/abs/2403.07816v1 — Branch-Train-MiX: Mixing Expert LLMs into a Mixture-of-Experts LLM
[2] https://arxiv.org/abs/2310.14774v2 — Principled Approaches for Learning to Defer with Multiple Experts
[3] https://arxiv.org/abs/2204.09179v3 — On the Representation Collapse of Sparse Mixture of Experts
[4] https://arxiv.org/abs/1711.05769v2 — PackNet: Adding Multiple Tasks to a Single Network by Iterative Pruning
[5] https://arxiv.org/abs/2406.13233v2 — AdaMoE: Token-Adaptive Routing with Null Experts for Mixture-of-Experts Language Models
[6] https://arxiv.org/abs/2402.04411v2 — DFA-RAG: Conversational Semantic Router for Large Language Model with Definite Finite Automaton
[7] https://arxiv.org/abs/2504.10561v2 — Self-Controlled Dynamic Expansion Model for Continual Learning
[8] https://arxiv.org/abs/2508.12801v1 — Maximum Score Routing For Mixture-of-Experts
[9] https://arxiv.org/abs/2607.07676v1 — SkillCenter: A Large-Scale Source-Grounded Skill Library for Autonomous AI Agents
[10] https://arxiv.org/abs/2604.03964v1 — SKILLFOUNDRY: Building Self-Evolving Agent Skill Libraries from Heterogeneous Scientific Resources
[11] https://arxiv.org/abs/2305.16291v2 — Voyager: An Open-Ended Embodied Agent with Large Language Models
[12] https://arxiv.org/abs/2308.00951v2 — From Sparse to Soft Mixtures of Experts
[13] https://arxiv.org/abs/2405.01563v1 — Mitigating LLM Hallucinations via Conformal Abstention
[14] https://arxiv.org/abs/2604.27914v1 — Geometry-Calibrated Conformal Abstention for Language Models
[15] https://arxiv.org/abs/2208.03753v1 — Learning Modular Structures That Generalize Out-of-Distribution
[16] https://arxiv.org/abs/2306.04640v2 — ModuleFormer: Modularity Emerges from Mixture-of-Experts
[17] https://arxiv.org/abs/2401.06066v1 — DeepSeekMoE: Towards Ultimate Expert Specialization in Mixture-of-Experts Language Models
[18] https://arxiv.org/abs/2202.09368v2 — Mixture-of-Experts with Expert Choice Routing
[19] https://arxiv.org/abs/1606.04671v4 — Progressive Neural Networks
[20] https://arxiv.org/abs/2607.27143v1 — Cost-Sensitive Conformal Prediction and Human-in-the-Loop Abstention for Imbalanced High-Stakes Decision Support: A Mult
[21] https://arxiv.org/abs/2501.15068v3 — An Atomic Skill Library Construction Method for Data-Efficient Embodied Manipulation
[22] https://arxiv.org/abs/2512.17102v2 — Reinforcement Learning for Self-Improving Agent with Skill Library
[23] https://arxiv.org/abs/2605.13716v1 — SkillOps: Managing LLM Agent Skill Libraries as Self-Maintaining Software Ecosystems
[24] https://arxiv.org/abs/2608.05604v1 — SkillZip: Contract-Preserving Graph Compression for Scalable Agent Skill Libraries
[25] https://arxiv.org/abs/2511.01854v2 — Tool-to-Agent Retrieval: Bridging Tools and Agents for Scalable LLM Multi-Agent Systems
[26] https://arxiv.org/abs/2606.20023v2 — When Lower Privileges Suffice: Investigating Over-Privileged Tool Selection in LLM Agents
[27] https://arxiv.org/abs/2603.05786v2 — Proof-of-Guardrail in AI Agents and What (Not) to Trust from It
[28] https://arxiv.org/abs/2608.01050v1 — Don't Offer What Can't Be Done: Deterministic Executability Gating for LLM Skill Selection at Scale
[29] https://arxiv.org/abs/2608.02356v2 — SkillTrace: Traversing a Query-Skill Graph for Composable LLM Agents
[30] https://arxiv.org/abs/2608.04482v1 — Skills Know Their Neighbors: Cluster-Contrastive Capability Pages for Skill Retrieval
[31] https://arxiv.org/abs/2110.11334v3 — Generalized Out-of-Distribution Detection: A Survey
[32] https://arxiv.org/abs/2504.00441v2 — No Free Lunch with Guardrails
[33] https://arxiv.org/abs/2605.07073v1 — TeamBench: Evaluating Agent Coordination under Enforced Role Separation
[34] https://arxiv.org/abs/2005.00247v3 — AdapterFusion: Non-Destructive Task Composition for Transfer Learning
[35] https://arxiv.org/abs/2101.03961v3 — Switch Transformers: Scaling to Trillion Parameter Models with Simple and Efficient Sparsity
[36] https://arxiv.org/abs/2404.13628v1 — Mixture of LoRA Experts
[37] https://arxiv.org/abs/2604.15771v3 — Skill-RAG: Failure-State-Aware Retrieval Augmentation via Hidden-State Probing and Skill Routing
[38] https://arxiv.org/abs/2310.12690v2 — Neurosymbolic Grounding for Compositional World Models
[39] https://arxiv.org/abs/2608.06362v1 — AV-AIVAT: 74x Cheaper Agent Evaluation with Certified Anytime-Valid Stopping in Imperfect-Information Games
[40] https://arxiv.org/abs/2608.06370v1 — The Bitter Lesson of Tool Calling
[41] https://arxiv.org/abs/1711.06664v3 — Predict Responsibly: Improving Fairness and Accuracy by Learning to Defer
[42] https://arxiv.org/abs/2605.27955v1 — Skill-as-Pseudocode: Refactoring Skill Libraries to Pseudocode for LLM Agents
[43] https://arxiv.org/abs/2608.04084v1 — SpecDrop: Parameter-Free Category-Conditioned Routing for Modular Specialization
[44] https://arxiv.org/abs/2608.06374v1 — DyPES-VLA: Learning Shared Dynamics Priors and Embodiment-Specific Control for Cross-Embodiment Manipulation
[45] https://arxiv.org/abs/2204.01691v2 — Do As I Can, Not As I Say: Grounding Language in Robotic Affordances
[46] https://arxiv.org/abs/2502.06975v1 — Position: Episodic Memory is the Missing Piece for Long-Term LLM Agents
[47] https://arxiv.org/abs/2608.03874v1 — ContinualSkillBench: Can LLM Agents Truly Evolve Their Capabilities?
[48] https://arxiv.org/abs/2608.04401v1 — Elbow-Based MoE Routing: A Training-Free Inference Time Plugin for Expert Selection
