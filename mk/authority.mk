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
capsule_frontdoor: cnetd capsule_core tests/test_capsule_frontdoor.sh tests/test_capsule_socket_client.c
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_capsule_socket_client tests/test_capsule_socket_client.c
	@bash tests/test_capsule_frontdoor.sh | tee logs/capsule_frontdoor.log
	@grep -q '^CAPSULE_FRONTDOOR_PASS' logs/capsule_frontdoor.log

$(BIN_DIR)/cnet_revalidate_coverage: tools/cnet_revalidate_coverage.c $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ tools/cnet_revalidate_coverage.c \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

capsule_core: $(BIN_DIR)/cnet_capsule_core
$(BIN_DIR)/cnet_capsule_core: tools/cnet_capsule_core_main.c $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(CFLAGS) -Werror -o $@ tools/cnet_capsule_core_main.c \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

$(BIN_DIR)/libcnet_capsule_core.so: mk/authority.mk $(LIBCCE) $(AUTHORITY_CORE_SRC) $(CAPSULE_SRC) src/memory/cnet_semantic_cortex.c src/memory/cnet_shared_workspace.c src/serve/cnet_capsule_core.c src/serve/cnet_capsule_demand.c $(wildcard include/*.h include/*/*.h)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $(AUTHORITY_CORE_SRC) $(CAPSULE_SRC) \
		src/serve/cnet_capsule_core.c src/serve/cnet_capsule_demand.c $(SEMANTIC_CORTEX_SRC) $(SHARED_WORKSPACE_SRC) $(LIBCCE) $(LDFLAGS) $(MCP_LDFLAGS) -pthread

capsule_core_growth: $(BIN_DIR)/libcnet_capsule_core.so
capsule_core_growth: tests/test_capsule_core_growth.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_capsule_core_growth \
		tests/test_capsule_core_growth.c -L$(BIN_DIR) -lcnet_capsule_core \
		-Wl,-rpath,'$$ORIGIN' $(LDFLAGS) $(MCP_LDFLAGS) -ldl -pthread
	@$(BIN_DIR)/test_capsule_core_growth | tee logs/capsule_core_growth.log
	@grep -q '^CAPSULE_CORE_GROWTH_PASS' logs/capsule_core_growth.log

.PHONY: personal_ai_reroute
personal_ai_reroute: $(LIBCCE) tests/test_personal_ai_reroute.c $(AUTHORITY_CORE_SRC)
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_personal_ai_reroute \
		$(AUTHORITY_CORE_SRC) tests/test_personal_ai_reroute.c $(LIBCCE) \
		$(LDFLAGS) $(MCP_LDFLAGS) -pthread
	@$(BIN_DIR)/test_personal_ai_reroute | tee logs/personal_ai_reroute.log
