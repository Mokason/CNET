# Capability evidence and runner integrity.
.PHONY: capability_cert_runner_test heldout_fixture_test capability_evaluator_prereq capability_fixture_causality capability_cert

capability_cert_runner_test: dotnet/CnetControlPlane/CnetControlPlane.csproj dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj
	@mkdir -p logs
	@$(BIN_DIR)/gate_evidence capability_cert_runner_test \
		logs/capability_cert_runner.log CAPABILITY_CERT_RUNNER_PASS -- \
		bash scripts/run_dotnet_gate.sh CAPABILITY_CERT_RUNNER_PASS \
		dotnet test dotnet/CnetControlPlane.Tests \
		--filter FullyQualifiedName~CapabilityCertRunnerTests --verbosity minimal

# Unit gate for the fixture reader every evaluator now certifies through. If it
# can be made to report a fixture it did not consume, every causality claim
# downstream is decorative again, so its negatives are the point.
heldout_fixture_test: $(HELDOUT_SRC) tests/test_cnet_heldout.c include/cnet_heldout.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/$@ \
		$(HELDOUT_SRC) tests/test_cnet_heldout.c $(LDFLAGS)
	@$(BIN_DIR)/gate_evidence heldout_fixture_test \
		logs/heldout_fixture_test.log CNET_HELDOUT_TEST_PASS -- \
		$(BIN_DIR)/heldout_fixture_test

# A fresh checkout must be able to BUILD what it is about to believe. This
# exists because `make capability_cert` passed here and exited 2 in a fresh
# detached worktree of the same commit: `dotnet test --no-restore` against an
# unrestored project emits zero bytes and exits 0, and nothing in the tree ever
# built it. The unit lane proves the guard's negatives; the CLI prepares every
# committed manifest and refuses missing or stale output by name.
capability_evaluator_prereq: dotnet/CnetControlPlane/CnetControlPlane.csproj \
		dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj \
		$(wildcard config/capability_manifests/*.json)
	@mkdir -p logs
	@$(BIN_DIR)/gate_evidence capability_evaluator_prereq \
		logs/capability_evaluator_prereq.log \
		CAPABILITY_EVALUATOR_PREREQ_UNIT_PASS -- \
		bash scripts/run_dotnet_gate.sh CAPABILITY_EVALUATOR_PREREQ_UNIT_PASS \
		dotnet test dotnet/CnetControlPlane.Tests --filter FullyQualifiedName~EvaluatorPrereqTests --verbosity minimal

# The decisive truthfulness experiment: mutate one declared expectation while
# leaving every marker string byte-identical, and require the evaluator to fail.
capability_fixture_causality: dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj \
		dotnet/CnetControlPlane/CnetControlPlane.csproj
	@mkdir -p logs
	@$(BIN_DIR)/gate_evidence capability_fixture_causality \
		logs/capability_fixture_causality.log \
		CAPABILITY_FIXTURE_CAUSALITY_PASS -- \
		bash scripts/run_dotnet_gate.sh CAPABILITY_FIXTURE_CAUSALITY_PASS \
		dotnet test dotnet/CnetControlPlane.Tests --filter FullyQualifiedName~FixtureCausalityCoreTests --verbosity minimal

capability_cert: capability_cert_runner_test capability_evaluator_prereq heldout_fixture_test capability_fixture_causality
	@dotnet run --project dotnet/CnetControlPlane -- capability-cert

capability_cert_runner_test capability_evaluator_prereq heldout_fixture_test capability_fixture_causality: | $(BIN_DIR)/gate_evidence

.PHONY: capability_runner_status cce_build_dependencies
capability_runner_status: $(BIN_DIR)/gate_evidence
	@mkdir -p logs
	@bash tests/test_capability_runner_status.sh | tee logs/capability_runner_status.log

cce_build_dependencies:
	@mkdir -p logs
	@bash tests/test_cce_build_dependencies.sh | tee logs/cce_build_dependencies.log
