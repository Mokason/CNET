# Actual serving/learning authority integration regressions.
.PHONY: capsule_large_inventory_bench
.PHONY: capsule_selected_audit
capsule_selected_audit: $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_capsule_selected_audit tests/test_capsule_selected_audit.c -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
	@$(BIN_DIR)/test_capsule_selected_audit | tee logs/capsule_selected_audit.log
	@grep -q '^CAPSULE_SELECTED_AUDIT_PASS' logs/capsule_selected_audit.log
authority: capsule_selected_audit
$(BIN_DIR)/test_capsule_large_inventory: tests/test_capsule_large_inventory.c $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ $< -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
capsule_large_inventory_bench: $(BIN_DIR)/test_capsule_large_inventory $(BIN_DIR)/cnet_capsule_core $(BIN_DIR)/cnet_capsule_tool $(BIN_DIR)/cnet_capsule_scale_probe
	@bash scripts/cnet_capsule_large_inventory_bench.sh
.PHONY: capsule_scale_bench capsule_scale_contract
$(BIN_DIR)/cnet_capsule_scale_probe: tools/cnet_capsule_scale_probe.c include/cnet_capsule_core.h $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ $< -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
capsule_scale_contract: $(BIN_DIR)/cnet_capsule_scale_probe $(BIN_DIR)/cnet_capsule_core $(BIN_DIR)/cnet_capsule_tool
	@bash tests/test_capsule_scale_bench.sh
capsule_scale_bench: $(BIN_DIR)/cnet_capsule_scale_probe $(BIN_DIR)/cnet_capsule_core $(BIN_DIR)/cnet_capsule_tool
	@bash scripts/cnet_capsule_scale_bench.sh
authority: capsule_scale_contract
.PHONY: capsule_capacity
$(BIN_DIR)/test_capsule_replay_scale: tests/test_capsule_replay_scale.c src/serve/cnet_capsule_core.c $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ $< -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
capsule_capacity: $(BIN_DIR)/cnet_capsule_scale_probe $(BIN_DIR)/cnet_capsule_core $(BIN_DIR)/cnet_capsule_tool $(BIN_DIR)/test_capsule_replay_scale
	@bash tests/test_capsule_capacity.sh
authority: capsule_capacity
.PHONY: authority
authority: capability_runner_status cce_build_dependencies core_serve_authority \
		personal_ai_reroute evolve_authority core_bus_untagged core_bus_tensor_refusal \
		cnetd_protocol_boundary cnet_mcp_transport roe_process_security capsule_core_growth capsule_frontdoor
	@echo CNET_AUTHORITY_PASS | tee logs/authority.log
authority: base_quarantine
authority: capsule_tool capsule_demand_growth
authority: capsule_demand_security capsule_acquire_security
authority: capsule_core_budget
.PHONY: capsule_core_budget
capsule_core_budget: $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_capsule_core_budget tests/test_capsule_core_budget.c -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
	@$(BIN_DIR)/test_capsule_core_budget | tee logs/capsule_core_budget.log
.PHONY: capsule_demand_security capsule_acquire_security
capsule_demand_security: $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_capsule_demand_security tests/test_capsule_demand_security.c -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN'
	@$(BIN_DIR)/test_capsule_demand_security | tee logs/capsule_demand_security.log
capsule_acquire_security: capsule_core capsule_tool
	@bash tests/test_capsule_acquire_security.sh | tee logs/capsule_acquire_security.log
.PHONY: capsule_tool capsule_demand_growth
$(BIN_DIR)/cnet_capsule_tool: tools/cnet_capsule_tool.c tools/cnet_capsule_tool_plan.c tools/cnet_capsule_tool_internal.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -Werror -o $@ tools/cnet_capsule_tool.c tools/cnet_capsule_tool_plan.c
authority: capsule_tool_plan
authority: capsule_composed_acquire capsule_composed_refusal
.PHONY: capsule_composed_acquire capsule_composed_refusal
capsule_composed_acquire: capsule_demand_growth capsule_tool_plan
	@bash tests/test_capsule_composed_acquire.sh | tee logs/capsule_composed_acquire.log
capsule_composed_refusal: capsule_core capsule_tool_plan
	@bash tests/test_capsule_composed_refusal.sh | tee logs/capsule_composed_refusal.log
.PHONY: capsule_tool_plan
capsule_tool_plan: capsule_tool
	@bash tests/test_capsule_tool_plan.sh | tee logs/capsule_tool_plan.log
capsule_tool: $(BIN_DIR)/cnet_capsule_tool
	@mkdir -p logs
	@bash tests/test_capsule_tool.sh | tee logs/capsule_tool.log
.PHONY: capsule_fresh_build
capsule_fresh_build:
	@bash tests/test_capsule_fresh_build.sh
authority: capsule_fresh_build
capsule_demand_growth: capsule_frontdoor capsule_tool
	@bash tests/test_capsule_demand_growth.sh | tee logs/capsule_demand_growth.log
authority: capsule_value_search capsule_history capsule_history_coverage
.PHONY: capsule_value_search capsule_history capsule_history_coverage
capsule_value_search: capsule_core
	@bash tests/test_capsule_value_search.sh | tee logs/capsule_value_search.log
capsule_history: capsule_core
	@bash tests/test_capsule_history.sh | tee logs/capsule_history.log
capsule_history_coverage: $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_capsule_history_coverage tests/test_capsule_history_coverage.c -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
	@$(BIN_DIR)/test_capsule_history_coverage | tee logs/capsule_history_coverage.log
authority: capsule_finite_compile
.PHONY: capsule_finite_compile
capsule_finite_compile: capsule_core
	@bash tests/test_capsule_finite_compile.sh | tee logs/capsule_finite_compile.log
	@grep -q '^CAPSULE_FINITE_COMPILE_PASS' logs/capsule_finite_compile.log
authority: capsule_publication_interfaces
.PHONY: capsule_publication_interfaces
capsule_publication_interfaces: capsule_core
	@bash tests/test_capsule_publication_interfaces.sh | tee logs/capsule_publication_interfaces.log
	@grep -q '^CAPSULE_PUBLICATION_INTERFACES_PASS' logs/capsule_publication_interfaces.log
authority: lane_maintenance
.PHONY: lane_maintenance
lane_maintenance:
	@bash tests/test_lane_maintenance.sh | tee logs/lane_maintenance.log
	@grep -q '^LANE_MAINTENANCE_PASS' logs/lane_maintenance.log
authority: capsule_intent
.PHONY: capsule_intent
capsule_intent:
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_capsule_intent tests/test_capsule_intent.c $(SEMANTIC_CORTEX_SRC) $(SHARED_WORKSPACE_SRC) $(LDFLAGS)
	@$(BIN_DIR)/test_capsule_intent | tee logs/capsule_intent.log
	@grep -q '^CAPSULE_INTENT_PASS' logs/capsule_intent.log
authority: capsule_curriculum
.PHONY: capsule_curriculum
capsule_curriculum: capsule_core
	@bash tests/test_capsule_curriculum.sh | tee logs/capsule_curriculum.log
	@grep -q '^CAPSULE_CURRICULUM_PASS' logs/capsule_curriculum.log
authority: roe_publication
authority: roe_frontdoor_publication
.PHONY: roe_frontdoor_publication
roe_frontdoor_publication: $(BIN_DIR)/roe_front_door
	@bash tests/test_roe_frontdoor_publication.sh | tee logs/roe_frontdoor_publication.log
	@grep -q '^ROE_FRONTDOOR_PUBLICATION_PASS' logs/roe_frontdoor_publication.log
.PHONY: authority_fixture_isolation
authority: authority_fixture_isolation
authority_fixture_isolation:
	@bash tests/test_authority_fixture_isolation.sh | tee logs/authority_fixture_isolation.log
.PHONY: roe_publication
roe_publication: $(ROE_ASI_SRC) tests/test_roe_publication.c
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_roe_publication tests/test_roe_publication.c $(ROE_ASI_SRC) $(LDFLAGS)
	@$(BIN_DIR)/test_roe_publication | tee logs/roe_publication.log
	@grep -q '^ROE_PUBLICATION_PASS' logs/roe_publication.log
authority: struct_mine_persistence
authority: checkpoint_binding
$(BIN_DIR)/libcnet_capsule_core.so: include/cnet_checkpoint_internal.h include/cnet_platform.h include/base.h
own_learning_health struct_mine_persist cnet_cert_learn_tick: include/cnet_checkpoint_internal.h
authority: capsule_socket_bench_contract
.PHONY: capsule_socket_bench_contract
$(BIN_DIR)/test_capsule_socket_client: tests/test_capsule_socket_client.c
	$(CC) $(CFLAGS) -Werror -o $@ $<
capsule_socket_bench_contract: $(BIN_DIR)/cnet_capsule_scale_probe $(BIN_DIR)/cnet_capsule_core $(BIN_DIR)/cnet_capsule_tool $(BIN_DIR)/cnetd $(BIN_DIR)/test_capsule_socket_client
	@bash tests/test_capsule_socket_bench.sh
authority: checkpoint_durability
authority: capsule_publish_recovery
.PHONY: capsule_publish_recovery
capsule_publish_recovery: $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_capsule_publish_recovery tests/test_capsule_publish_recovery.c -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
	@$(BIN_DIR)/test_capsule_publish_recovery | tee logs/capsule_publish_recovery.log
	@grep -q '^CAPSULE_PUBLISH_RECOVERY_PASS' logs/capsule_publish_recovery.log
.PHONY: checkpoint_durability
checkpoint_durability: $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_checkpoint_durability tests/test_checkpoint_durability.c -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
	@$(BIN_DIR)/test_checkpoint_durability | tee logs/checkpoint_durability.log
	@grep -q '^CHECKPOINT_DURABILITY_PASS' logs/checkpoint_durability.log
.PHONY: checkpoint_binding
checkpoint_binding: $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_checkpoint_binding tests/test_checkpoint_binding.c -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
	@$(BIN_DIR)/test_checkpoint_binding | tee logs/checkpoint_binding.log
	@grep -q '^CHECKPOINT_BINDING_PASS' logs/checkpoint_binding.log
.PHONY: struct_mine_persistence
struct_mine_persistence: $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_struct_mine_persistence tests/test_struct_mine_persistence.c -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
	@$(BIN_DIR)/test_struct_mine_persistence | tee logs/struct_mine_persistence.log
	@grep -q '^STRUCT_MINE_PERSISTENCE_PASS' logs/struct_mine_persistence.log
.PHONY: base_quarantine
base_quarantine: $(BIN_DIR)/cnet_quarantine_base
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_base_quarantine tests/test_base_quarantine.c -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
	@$(BIN_DIR)/test_base_quarantine | tee logs/base_quarantine.log
	@grep -q '^BASE_QUARANTINE_PASS' logs/base_quarantine.log

$(BIN_DIR)/cnet_quarantine_base: tools/cnet_quarantine_base.c $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ $< -L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
.PHONY: evolve_authority
evolve_authority: tests/test_evolve_authority.c tools/roe_evolve_tick.c include/cnet_json_internal.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -Iinclude -o $(BIN_DIR)/test_evolve_authority tests/test_evolve_authority.c
	@$(BIN_DIR)/test_evolve_authority | tee logs/evolve_authority.log
	@grep -q '^EVOLVE_AUTHORITY_PASS' logs/evolve_authority.log

$(BIN_DIR)/roe_evolve_tick: include/cnet_json_internal.h
cnetd_protocol_boundary cnetd: include/cnet_json_internal.h

.PHONY: core_serve_authority
core_serve_authority: tests/test_core_serve_authority.c src/serve/cnet_core_serve.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_core_serve_authority \
		tests/test_core_serve_authority.c src/serve/cnet_core_serve.c $(LDFLAGS)
	@$(BIN_DIR)/test_core_serve_authority | tee logs/core_serve_authority.log

AUTHORITY_CORE_SRC = $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY)

.PHONY: capsule_core_growth
.PHONY: capsule_core
.PHONY: capsule_frontdoor
capsule_frontdoor: cnetd capsule_core $(BIN_DIR)/cnet_capsulectl tests/test_capsule_frontdoor.sh tests/test_capsule_socket_client.c
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_capsule_socket_client tests/test_capsule_socket_client.c
	@bash tests/test_capsule_frontdoor.sh | tee logs/capsule_frontdoor.log
	@grep -q '^CAPSULE_FRONTDOOR_PASS' logs/capsule_frontdoor.log

.PHONY: capsule_control
$(BIN_DIR)/cnet_capsulectl: tools/cnet_capsulectl.c src/serve/cnet_capsule_snapshot.h $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -D_GNU_SOURCE -Werror -Isrc/serve -o $@ tools/cnet_capsulectl.c \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

$(BIN_DIR)/cnet_table_verify: tools/cnet_table_verify.c include/cnet_capsule_core.h include/cnet_capsule_table.h include/cnet_learning_sandbox.h src/serve/cnet_capsule_snapshot.h $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -D_GNU_SOURCE -Werror -Isrc/serve -o $@ tools/cnet_table_verify.c \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

$(BIN_DIR)/cnet_learning_snapshot: tools/cnet_learning_snapshot.c src/serve/cnet_capsule_snapshot.h $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -D_GNU_SOURCE -Werror -Isrc/serve -o $@ tools/cnet_learning_snapshot.c \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
capsule_control: $(BIN_DIR)/cnet_capsulectl
	@mkdir -p logs
	@python3 tests/test_capsulectl.py > logs/capsule_control.log 2>&1 || { cat logs/capsule_control.log; exit 1; }
	@cat logs/capsule_control.log
authority: capsule_control

$(BIN_DIR)/cnet_revalidate_coverage: tools/cnet_revalidate_coverage.c $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ tools/cnet_revalidate_coverage.c \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

capsule_core: $(BIN_DIR)/cnet_capsule_core
$(BIN_DIR)/cnet_capsule_core: tools/cnet_capsule_core_main.c $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ tools/cnet_capsule_core_main.c \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

CORE_CANDIDATE_SRC = src/serve/cnet_core_cell.c src/serve/cnet_core_selector.c src/serve/cnet_cell_capsule.c src/serve/cnet_core_candidate.c src/serve/cnet_core_host.c src/serve/cnet_capsule_snapshot.c src/serve/cnet_capsule_store.c src/serve/cnet_capsule_evidence.c src/serve/cnet_capsule_table.c src/serve/cnet_learning_sandbox.c src/cce/cce_campaign_provenance.c
$(BIN_DIR)/libcnet_capsule_core.so: mk/authority.mk $(LIBCCE) $(AUTHORITY_CORE_SRC) $(CAPSULE_SRC) $(CORE_CANDIDATE_SRC) src/memory/cnet_semantic_cortex.c src/memory/cnet_shared_workspace.c src/serve/cnet_capsule_core.c src/serve/cnet_capsule_demand.c $(wildcard include/*.h include/*/*.h)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $(AUTHORITY_CORE_SRC) $(CAPSULE_SRC) \
		src/serve/cnet_capsule_core.c src/serve/cnet_capsule_demand.c $(CORE_CANDIDATE_SRC) $(SEMANTIC_CORTEX_SRC) $(SHARED_WORKSPACE_SRC) $(LIBCCE) $(LDFLAGS) $(MCP_LDFLAGS) -pthread

capsule_core_growth: $(BIN_DIR)/libcnet_capsule_core.so
capsule_core_growth: tests/test_capsule_core_growth.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_capsule_core_growth \
		tests/test_capsule_core_growth.c -L$(BIN_DIR) -lcnet_capsule_core \
		-Wl,-rpath,'$$ORIGIN' $(LDFLAGS) $(MCP_LDFLAGS) -ldl -pthread
	@$(BIN_DIR)/test_capsule_core_growth | tee logs/capsule_core_growth.log
	@grep -q '^CAPSULE_CORE_GROWTH_PASS' logs/capsule_core_growth.log

.PHONY: capsule_resident_lifecycle
capsule_resident_lifecycle: $(BIN_DIR)/libcnet_capsule_core.so tests/test_capsule_resident_lifecycle.c tests/test_knowledge_capsule.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_capsule_resident_lifecycle tests/test_capsule_resident_lifecycle.c \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS) -ldl -pthread
	@$(BIN_DIR)/test_capsule_resident_lifecycle > logs/capsule_resident_lifecycle.log
	@grep '^CAPSULE_RESIDENT_PASS' logs/capsule_resident_lifecycle.log

.PHONY: capsule_snapshot capsule_store capsule_store_faults capsule_store_recovery capsule_daemon_lifecycle source_reserved_interface capsule_product_closure
capsule_snapshot: tests/test_capsule_snapshot.c
capsule_snapshot capsule_store: capsule_%: $(BIN_DIR)/libcnet_capsule_core.so tests/test_capsule_%.c tests/test_knowledge_capsule.c
	@mkdir -p logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_$@ tests/test_$@.c \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS) -ldl -pthread
	@$(BIN_DIR)/test_$@ > logs/$@.log
	@grep '^CAPSULE_.*_PASS' logs/$@.log

capsule_store_faults: $(BIN_DIR)/libcnet_capsule_core.so tests/test_capsule_store_faults.c tests/test_knowledge_capsule.c
	@mkdir -p logs
	$(CC) $(CFLAGS) -DCNET_CAPSULE_STORE_TESTING -o $(BIN_DIR)/test_capsule_store_faults \
		tests/test_capsule_store_faults.c src/serve/cnet_capsule_store.c \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS) -ldl -pthread
	@$(BIN_DIR)/test_capsule_store_faults > logs/capsule_store_faults.log
	@grep '^CAPSULE_STORE_FAULTS_PASS' logs/capsule_store_faults.log

source_reserved_interface: $(BIN_DIR)/libcnet_capsule_core.so tests/test_source_reserved_interface.c tests/test_knowledge_capsule.c
	@mkdir -p logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_source_reserved_interface tests/test_source_reserved_interface.c \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS) -ldl -pthread
	@$(BIN_DIR)/test_source_reserved_interface > logs/source_reserved_interface.log
	@grep '^SOURCE_RESERVED_PASS' logs/source_reserved_interface.log

capsule_daemon_lifecycle: cnetd capsule_core $(BIN_DIR)/cnet_source_capsule \
		tests/source_freshness_mutation_shim.c tests/capsule_owner_stat_shim.c
	@mkdir -p logs
	@python3 tests/test_capsule_daemon_lifecycle.py > logs/capsule_daemon_lifecycle.log 2>&1 || { cat logs/capsule_daemon_lifecycle.log; exit 1; }
	@cat logs/capsule_daemon_lifecycle.log

capsule_store_recovery: cnetd capsule_core tests/test_capsule_store_recovery.py tests/test_capsule_daemon_lifecycle.py
	@mkdir -p logs
	@python3 tests/test_capsule_store_recovery.py > logs/capsule_store_recovery.log 2>&1 || { cat logs/capsule_store_recovery.log; exit 1; }
	@cat logs/capsule_store_recovery.log

capsule_product_closure: capsule_resident_lifecycle capsule_snapshot capsule_store capsule_store_faults capsule_store_recovery source_evidence source_reserved_interface capsule_daemon_lifecycle
authority: capsule_product_closure

# Instrument the complete linked C runtime in a fresh private directory. Do not
# change shared CCE object flags or overwrite a developer's normal binaries.
.PHONY: capsule_product_sanitize
capsule_product_sanitize:
	@set -eu; capsule_san=$$(mktemp -d /tmp/cnet-product-sanitize-XXXXXX); \
	printf 'CAPSULE_SANITIZE_ARTIFACTS=%s\n' "$$capsule_san"; \
	$(CC) $(filter-out -O% -march=%,$(CFLAGS)) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fPIC -shared \
		-o "$$capsule_san/libcnet_capsule_core.so" $(AUTHORITY_CORE_SRC) $(CAPSULE_SRC) \
		src/serve/cnet_capsule_core.c src/serve/cnet_capsule_demand.c $(CORE_CANDIDATE_SRC) \
		$(SEMANTIC_CORTEX_SRC) $(SHARED_WORKSPACE_SRC) $(CCE_C_SRCS) $(LDFLAGS) $(MCP_LDFLAGS) -pthread \
		> "$$capsule_san/build.log" 2>&1 || { tail -n 60 "$$capsule_san/build.log"; exit 1; }; \
	for capsule_test in capsule_resident_lifecycle capsule_snapshot capsule_store capsule_store_faults source_reserved_interface; do \
		capsule_extra=''; if test "$$capsule_test" = capsule_store_faults; then capsule_extra='-DCNET_CAPSULE_STORE_TESTING src/serve/cnet_capsule_store.c'; fi; \
		$(CC) $(filter-out -O% -march=%,$(CFLAGS)) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
			-o "$$capsule_san/test_$$capsule_test" "tests/test_$$capsule_test.c" $$capsule_extra \
			-L"$$capsule_san" -lcnet_capsule_core -Wl,-rpath,"$$capsule_san" $(LDFLAGS) -ldl -pthread \
			> "$$capsule_san/$$capsule_test-build.log" 2>&1 || { tail -n 60 "$$capsule_san/$$capsule_test-build.log"; exit 1; }; \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
			CNET_TEST_CORE_LIBRARY="$$capsule_san/libcnet_capsule_core.so" "$$capsule_san/test_$$capsule_test" \
			> "$$capsule_san/$$capsule_test.log" 2>&1 || { cat "$$capsule_san/$$capsule_test.log"; exit 1; }; \
		grep '_PASS checks=' "$$capsule_san/$$capsule_test.log"; \
	done; printf 'CAPSULE_PRODUCT_SANITIZE_PASS\n'

$(BIN_DIR)/cnet_source_capsule: tools/cnet_source_capsule.c $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ tools/cnet_source_capsule.c -L$(BIN_DIR) -lcnet_capsule_core \
		-Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

$(BIN_DIR)/cnet_table_capsule: tools/cnet_table_capsule.c include/cnet_capsule_table.h include/cnet_learning_sandbox.h $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ tools/cnet_table_capsule.c -L$(BIN_DIR) -lcnet_capsule_core \
		-Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

.PHONY: learning_native learning_managed learning_daemon
$(BIN_DIR)/test_learning_sandbox: tests/test_learning_sandbox.c include/cnet_learning_sandbox.h $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ tests/test_learning_sandbox.c -L$(BIN_DIR) -lcnet_capsule_core \
		-Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
$(BIN_DIR)/test_table_reader: tests/test_table_reader.c include/cnet_capsule_table.h $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ tests/test_table_reader.c -L$(BIN_DIR) -lcnet_capsule_core \
		-Wl,-rpath,'$$ORIGIN' $(LDFLAGS)
learning_native: $(BIN_DIR)/test_learning_sandbox $(BIN_DIR)/test_table_reader $(BIN_DIR)/cnet_table_capsule $(BIN_DIR)/cnet_table_verify $(BIN_DIR)/cnet_learning_snapshot
	@mkdir -p logs
	@$(BIN_DIR)/test_learning_sandbox > logs/learning_sandbox.log 2>&1 || { cat logs/learning_sandbox.log; exit 1; }
	@cat logs/learning_sandbox.log
	@$(BIN_DIR)/test_table_reader > logs/table_reader.log 2>&1 || { cat logs/table_reader.log; exit 1; }
	@cat logs/table_reader.log
	@python3 tests/test_table_capsule.py
	@python3 tests/test_table_verify.py
learning_managed:
	dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --no-restore \
		--filter 'FullyQualifiedName~Learning|FullyQualifiedName~LocalTableReference|FullyQualifiedName~NativeControlProtocol'
.PHONY: learning_soak_verify
learning_soak_verify:
	python3 tests/test_learning_soak.py
	dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --no-restore \
		--filter 'FullyQualifiedName~ThreeTruthfulSourceStages'
learning_daemon: cnetd $(BIN_DIR)/cnet_capsule_core learning_native $(BIN_DIR)/cnet_capsulectl
	@python3 tests/test_table_daemon.py TableLearningDaemon.test_table_acquisition_refresh_rollback_and_restart

.PHONY: learning_symbol capsule_distinct_scale_contract capsule_distinct_scale_bench
learning_symbol: learning_daemon
	@python3 tests/test_symbol_capsule.py
	@python3 tests/test_unicode17_symbol_tables.py
	dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --no-restore \
		--filter 'FullyQualifiedName~LearningSymbol|FullyQualifiedName~KnownFailedProbeSurvivesSubsequentControlFailureOrCancellation'
capsule_distinct_scale_contract: $(BIN_DIR)/libcnet_capsule_core.so
	@python3 tests/capsule_scale_contract.py
# Opt-in measurement; kept outside default build and CI latency budgets.
capsule_distinct_scale_bench: $(BIN_DIR)/libcnet_capsule_core.so
	@python3 scripts/capsule_scale_bench.py

.PHONY: source_evidence
source_evidence: $(BIN_DIR)/cnet_source_capsule tests/test_source_evidence.c
	@mkdir -p logs
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_source_evidence tests/test_source_evidence.c -ldl
	@$(BIN_DIR)/test_source_evidence $(BIN_DIR)/libcnet_capsule_core.so > logs/source_evidence.log
	@grep '^SOURCE_EVIDENCE_PASS' logs/source_evidence.log
	@python3 tests/test_source_capsule.py

.PHONY: personal_ai_reroute
personal_ai_reroute: $(LIBCCE) tests/test_personal_ai_reroute.c $(AUTHORITY_CORE_SRC)
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_personal_ai_reroute \
		$(AUTHORITY_CORE_SRC) tests/test_personal_ai_reroute.c $(LIBCCE) \
		$(LDFLAGS) $(MCP_LDFLAGS) -pthread
	@$(BIN_DIR)/test_personal_ai_reroute | tee logs/personal_ai_reroute.log
