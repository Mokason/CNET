# CNET Model Compression Improvement Plan (C-Only)

**Version**: 2.0  
**Date**: 2026-07-06  
**Status**: Active  
**Owner**: Marble (via Self-Development Cycle)  
**Constraint**: We only use C in CNET. All implementation must be in pure C (no Python bridges in core paths).

---

## 1. Executive Summary

**Goal**: Improve CNET so it can reliably compress large models (e.g. 9B Mythos-style) while:
- Preserving acceptable stylistic/creative quality
- Enabling 1M context via sparse per-specialist routing (not full KV cache)
- Reducing hallucination through counterfactual + activation verification

**Realism** (fact-checked against current CNET capabilities and research):
- High feasibility: Counterfactual Routing, Sparse KV selection, Activation verification
- Moderate feasibility: Narrative Coherence Contracts
- Low feasibility: Near-lossless creative quality on heavily post-trained models

---

## 2. Research Synthesis (Fact-Checked)

### 2.1 Key Papers

| Paper | Contribution | CNET Implication | Confidence |
|-------|--------------|------------------|------------|
| Narrative Flattening (2605.27878) | Post-training reduces moral ambiguity & folklore texture | Need explicit Narrative Coherence Contracts | High |
| Counterfactual Routing (2604.14246) | Compare chosen vs counterfactual routes | Directly implementable in C router | High |
| SamKV (2508.11661) | Sparse attention + selective recomputation | Enables context routing without full KV | High |
| Domain-Grounded Tiered Retrieval (2603.17872) | Multi-phase verification | Pattern for CNET verification layer | High |

### 2.2 Random Variables Framing (from memory)

- **Systematic component** → High leverage for CNET (contracts, routing, verification)
- **Irreducible noise** → Stylistic flattening in heavily post-trained models

---

## 3. Implementation Phases (C-Only)

### Phase 0: Foundation (C-Only)

**Todos**:
- [ ] Create this plan file (done)
- [ ] Create `plans/delegation_master_report.md`
- [ ] Create `plans/phase0_foundation.md` (this sub-task artifact)
- [ ] Create `references/` directory and copy key PDFs
- [ ] Add plan to Obsidian Research index

**Artifact**: `plans/phase0_foundation.md`

---

### Phase 1: Counterfactual Routing Head (C-Only)

**Goal**: Reduce hallucinations via counterfactual expert path comparison.

**Sub-Tasks**:

**1.1 Design**
- [ ] Define `CounterfactualRoute` struct in `include/cce/cce_router.h`
- [ ] Design `cce_router_verify_claim()` interface
- [ ] Write `docs/contracts/counterfactual_routing.md`

**1.2 Implementation (Pure C)**
- [ ] Implement `cce_router_sample_counterfactuals()` in C
- [ ] Add consistency scoring logic
- [ ] Integrate with existing CNET verification path

**1.3 Verification**
- [ ] Create `tests/router/counterfactual_test.c`
- [ ] Generate CNET testimony for any regressions
- [ ] Update `compound_memory.txt`

**Artifact**: `plans/phase1_counterfactual_routing.md`

---

### Phase 2: Sparse Per-Specialist KV Routing (C-Only)

**Goal**: Enable long context via routing instead of full KV cache.

**Sub-Tasks**:

**2.1 Research**
- [ ] Study SamKV paper
- [ ] Write `references/samkv_cnet_adaptation.md`

**2.2 Design (C)**
- [ ] Define `cce_specialist_kv_budget`
- [ ] Design `SparseKVSelector` in C

**2.3 Implementation (Pure C)**
- [ ] Implement `cce_specialist_select_kv_tokens()` in C
- [ ] Add routing mode configuration

**2.4 Verification**
- [ ] Test on long-context benchmarks
- [ ] Generate CNET testimony

**Artifact**: `plans/phase2_sparse_kv_routing.md`

---

### Phase 3: Narrative Coherence Contracts (C-Only)

**Goal**: Protect stylistic/creative quality.

**Sub-Tasks**:

**3.1 Contract Design (C)**
- [ ] Define `NarrativeCoherenceContract` struct in `include/contract/narrative_coherence.h`
- [ ] Implement scoring functions in C
- [ ] Create evaluation rubric

**3.2 Integration (Pure C)**
- [ ] Add `NARRATIVE_SPECIALIST` type
- [ ] Modify router in C to prefer narrative specialists on creative tasks

**3.3 Evaluation**
- [ ] Build internal creative writing test
- [ ] Compare original vs compressed
- [ ] Generate CNET testimony

**Artifact**: `plans/phase3_narrative_coherence.md` (already created)

---

### Phase 4: Uncertainty-Aware Activation (C-Only)

**Sub-Tasks**:

**4.1 Uncertainty (C)**
- [ ] Add `cce_specialist_get_uncertainty()` in C

**4.2 AICIMO Alignment (C)**
- [ ] Preserve backward path structure during compression (identity + residual)

**4.3 Hermes Preparation (C)**
- [ ] Document C-only hosting steps

**Artifact**: `plans/phase4_uncertainty_activation.md`

---

## 4. Verification Strategy (C-Only)

**Mandatory Artifacts for Every Sub-Task**:
1. `plans/<phase>_<name>.md`
2. CNET testimony for regressions
3. Update `compound_memory.txt`
4. Entry in `implementation_log.jsonl`

**Testing**:
- Pure C unit tests (`tests/`)
- Integration via existing CNET tools
- End-to-end via CnetMcpServer (when needed)

---

## 5. Risk Register

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Narrative contracts insufficient for creative quality | Medium | High | Accept partial gains; focus on structured tasks |
| Sparse KV introduces new bugs | Medium | Medium | Extensive testing + counterfactual checks |
| Implementation complexity in C | High | Medium | Subagent delegation for C work |

---

**This plan is now C-only and follows the TaskOrchestrator pattern with guaranteed per-sub-task artifacts.**

*Updated during 3-layer deep research cycle. Connected to AICIMO, Bayesian modeling, and RPG Storytelling memory.*