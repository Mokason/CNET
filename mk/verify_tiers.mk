# mk/verify_tiers.mk -- T0 / T1 / T2 membership for the verify ladder.
#
# One default command (`make verify` / `make test`). Everything else is a
# named soak, not a second "truth".
#
#   T0  verify-fast   edit loop — integrity smoke + light PEFT units
#   T1  verify        before push — build honesty + core runtime + contract law
#   T2  verify-t2     specialty CCE + PEFT soak (nightly / verify-long pull this)
#   long verify-long  T1+T2 + supra QAT + compat
#
# Rule for new pins: default to T2 unless the failure was a silent main-path
# lie (then T1). Every CORE row in tests/verify_logs.sh must be produced by a
# T1 dep (enforced by tests/verify_tier_sync.sh via build_integrity).
#
# WALL: each specialty CCE test re-links ~50 TU. Keep T1 to one clgemm proof +
# archive/forest/view/detect + contract chain; park the rest on T2.

# ---- T1: default verify -----------------------------------------------------
# Four jobs:
#   1. build integrity
#   2. core math/runtime (clgemm + archive/forest + model I/O + detect + leak)
#   3. contract law (secure/unit/heal/mutate/acquire/attribution/base/live_contracts)
#   4. ship-ish surface (flagship) + light PEFT smoke
VERIFY_T1_DEPS := \
	authority \
	build_integrity \
	clgemm_unit \
	cce_archive \
	cce_forest \
	mmap_read_identity \
	recipe_gate \
	json_escape \
	claims_test \
	cce_dll \
	cce_safetensors_test \
	cnet_lm_bounds_test \
	cce_autograd_test \
	cce_model_test \
	cce_view \
	forest_view \
	cce_detect \
	contract_secure \
	contract_unit \
	heal_mismatch \
	mutate \
	acquire \
	attribution \
	base \
	flagship \
	live_contracts \
	leakcheck \
	cnet_fault_test \
	cce_adapter_bank_test \
	cce_dora_test \
	cnet_serve_decode_test

# ---- T2: soak (not default) -------------------------------------------------
# Specialty CCE + heavy PEFT + distrust/autonomy soak.
VERIFY_T2_DEPS := \
	cce_ssm \
	cce_hybrid \
	cce_qwen35 \
	cce_st_llama \
	cce_specgraph \
	cce_wstore \
	cce_tiers \
	cce_similar \
	merge_family \
	hybrid_catalog \
	transformer_qat \
	qat_block \
	mojo_bridge \
	cnet_fault_loop_test \
	registry_lora_store_test \
	jtc_adapter_bench \
	metric_honesty \
	moe_ckpt_test \
	distrust_loop \
	autonomy_tick \
	autonomy_spine

VERIFY_SENTINEL := logs/.verify_sentinel
VERIFY_T2_SENTINEL := logs/.verify_t2_sentinel

.PHONY: verify verify_impl verify-fast verify-t2 verify_t2_impl
.PHONY: verify-long verify-long_impl verify-nightly
.PHONY: metric_honesty moe_ckpt_test

# T1 — default. Sentinel first (recursive make) so log freshness is honest.
verify:
	@mkdir -p logs
	@rm -f $(VERIFY_SENTINEL)
	@touch $(VERIFY_SENTINEL)
	@$(MAKE) --no-print-directory verify_impl
	@VERIFY_SINCE=$(VERIFY_SENTINEL) sh tests/verify_logs.sh

verify_impl: $(VERIFY_T1_DEPS)
	@:

# T0 — edit loop. No log-gate (fast feedback > ledger).
verify-fast: recipe_gate claims_test cce_dll clgemm_unit contract_unit \
		cnet_fault_test cce_adapter_bank_test cce_dora_test cnet_serve_decode_test
	@echo VERIFY_FAST_PASS

# T2 — specialty CCE + PEFT. Own sentinel + t2 log rows.
verify-t2:
	@mkdir -p logs
	@rm -f $(VERIFY_T2_SENTINEL)
	@touch $(VERIFY_T2_SENTINEL)
	@$(MAKE) --no-print-directory verify_t2_impl
	@VERIFY_SINCE=$(VERIFY_T2_SENTINEL) sh tests/verify_logs.sh t2
	@echo VERIFY_T2_PASS

verify_t2_impl: $(VERIFY_T2_DEPS)
	@:

# Long: T1 + T2 + supra QAT + compat.
verify-long:
	@$(MAKE) --no-print-directory verify-long_impl
	@VERIFY_SINCE=$(VERIFY_SENTINEL) sh tests/verify_logs.sh long

verify-long_impl: verify verify-t2 cce_train_bench supra_head_qat supra_head_qat_corpus \
		transformer_qat_joint wordlm_bitnet wordlm_holdout compat
	@:

# Nightly: T1 + T2 + openlab/grade/procedure + mutation probe.
verify-nightly: verify verify-t2 cnet_openlab_import cnet_grade_up cnet_a_grade \
		procedure_chunks metric_honesty_mutation
	@echo VERIFY_NIGHTLY_PASS

test: verify
