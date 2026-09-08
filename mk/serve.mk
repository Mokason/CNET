# Fast, model-free serving boundary gates.

.PHONY: core_brick_capacity
core_brick_capacity: tests/test_core_brick_capacity.c include/cnet_core_bus.h \
        include/cnet_core_serve.h src/serve/cnet_core_serve.c src/serve/cnet_core_bus.c \
        src/cnet_weight_convert.c src/router/registry.c $(CCE_GGUF)
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -ffunction-sections -fdata-sections -Iinclude \
		-o $(BIN_DIR)/test_core_brick_capacity tests/test_core_brick_capacity.c \
		src/serve/cnet_core_serve.c src/serve/cnet_core_bus.c src/cnet_weight_convert.c \
		src/router/registry.c $(CCE_GGUF) $(LDFLAGS) -Wl,--gc-sections
	@$(BIN_DIR)/test_core_brick_capacity | tee logs/core_brick_capacity.log
	@grep -q '^BRICK_CAPACITY_PASS' logs/core_brick_capacity.log

.PHONY: cnetd_brick_capacity
cnetd_brick_capacity: cnetd tests/test_cnetd_brick_capacity.py
	CNETD_BIN=$(BIN_DIR)/cnetd $(PYTHON) tests/test_cnetd_brick_capacity.py -v

.PHONY: core_evolve_brick_capacity
core_evolve_brick_capacity: cnet_core_evolve tests/test_core_evolve_brick_capacity.py
	CNET_EVOLVE_BIN=$(BIN_DIR)/cnet_core_evolve $(PYTHON) tests/test_core_evolve_brick_capacity.py -v

.PHONY: core_bus_untagged
core_bus_untagged: tests/test_core_bus_untagged.c src/serve/cnet_core_bus.c \
		include/cnet_core_bus.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -ffunction-sections -fdata-sections -Iinclude \
		-o $(BIN_DIR)/test_core_bus_untagged \
		tests/test_core_bus_untagged.c src/serve/cnet_core_bus.c \
		$(LDFLAGS) -Wl,--gc-sections
	@./$(BIN_DIR)/test_core_bus_untagged | tee logs/core_bus_untagged.log
	@grep -q '^CORE_BUS_UNTAGGED_PASS' logs/core_bus_untagged.log
	@grep -q 'unaddressed_claims_cert=0' logs/core_bus_untagged.log

.PHONY: core_bus_tensor_refusal
core_bus_tensor_refusal: tests/test_core_bus_tensor_refusal.c \
		src/serve/cnet_core_bus.c include/cnet_core_bus.h $(CCE_GGUF)
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -ffunction-sections -fdata-sections -Iinclude \
		-o $(BIN_DIR)/test_core_bus_tensor_refusal \
		tests/test_core_bus_tensor_refusal.c src/serve/cnet_core_bus.c \
		$(CCE_GGUF) $(LDFLAGS) -Wl,--gc-sections
	@./$(BIN_DIR)/test_core_bus_tensor_refusal | tee logs/core_bus_tensor_refusal.log
	@grep -q '^CORE_BUS_TENSOR_REFUSAL_PASS' logs/core_bus_tensor_refusal.log
	@grep -q 'explicit_miss_fallback=0' logs/core_bus_tensor_refusal.log

.PHONY: cnetd_protocol_boundary
cnetd_protocol_boundary: tests/test_cnetd_protocol.c include/cnetd_protocol.h \
		src/serve/cnetd_protocol.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnetd_protocol \
		tests/test_cnetd_protocol.c src/serve/cnetd_protocol.c $(LDFLAGS)
	@./$(BIN_DIR)/test_cnetd_protocol | tee logs/cnetd_protocol_boundary.log
	@grep -q '^CNETD_PROTOCOL_BOUNDARY_PASS' logs/cnetd_protocol_boundary.log

.PHONY: cnet_mcp_transport
cnet_mcp_transport: tests/test_cnet_mcp_transport.c \
		src/serve/cnet_mcp_client.c include/cnet_mcp_client.h \
		include/cnet_mcp_evidence_internal.h include/cnet_json_internal.h \
		src/cce/cce_campaign_provenance.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(filter-out -DCNET_HAVE_CURL=%,$(CFLAGS)) \
		-DCNET_HAVE_CURL=0 -Werror -Iinclude \
		-Wl,--wrap=send -o $(BIN_DIR)/test_cnet_mcp_transport \
		tests/test_cnet_mcp_transport.c src/serve/cnet_mcp_client.c src/cce/cce_campaign_provenance.c \
		$(LDFLAGS)
	@./$(BIN_DIR)/test_cnet_mcp_transport | tee logs/cnet_mcp_transport.log
	@grep -q '^MCP_CLIENT_TRANSPORT_PASS' logs/cnet_mcp_transport.log

.PHONY: mcp_read_protocol mcp_read_brick
mcp_read_protocol: tests/test_mcp_read_protocol_main.c tests/test_mcp_read_protocol.py \
		src/serve/cnet_mcp_client.c include/cnet_mcp_client.h \
		include/cnet_mcp_evidence_internal.h include/cnet_json_internal.h \
		src/cce/cce_campaign_provenance.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(filter-out -DCNET_HAVE_CURL=%,$(CFLAGS)) -DCNET_HAVE_CURL=0 \
		-Werror -Iinclude tests/test_mcp_read_protocol_main.c \
		src/serve/cnet_mcp_client.c src/cce/cce_campaign_provenance.c \
		-o $(BIN_DIR)/test_mcp_read_protocol $(LDFLAGS)
	$(PYTHON) tests/test_mcp_read_protocol.py -v

$(BIN_DIR)/cnet_mcp_read: tools/cnet_mcp_read_main.c src/serve/cnet_mcp_read_brick.c \
		src/serve/cnet_mcp_client.c include/cnet_mcp_read_brick.h \
		include/cnet_mcp_client.h include/cnet_mcp_evidence_internal.h \
		include/cnet_json_internal.h $(BIN_DIR)/libcnet_capsule_core.so
	$(CC) $(filter-out -DCNET_HAVE_CURL=%,$(CFLAGS)) -DCNET_HAVE_CURL=0 \
		-Werror -Iinclude tools/cnet_mcp_read_main.c src/serve/cnet_mcp_read_brick.c \
		src/serve/cnet_mcp_client.c -o $@ $(LDFLAGS) \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN'

mcp_read_brick: $(BIN_DIR)/cnet_mcp_read capsule_core mcp_read_protocol
	$(PYTHON) tests/test_cnet_mcp_read_brick.py -v

.PHONY: cnetd_mcp_read
cnetd_mcp_read: cnetd
	CNETD_BIN=$(BIN_DIR)/cnetd $(PYTHON) tests/test_cnetd_mcp_read.py -v

# Default integration is hermetic; public Wikipedia requires an explicit
# CNET_MCP_LIVE_TEST=1 when running tests/test_mcp_read_integration.py.
.PHONY: mcp_read_verify
mcp_read_verify: mcp_read_brick cnetd_mcp_read cnet_mcp_transport soul_host_test
	dotnet build dotnet/CnetMcpServer.Tests/CnetMcpServer.Tests.csproj -t:Rebuild --nologo -v:q
	dotnet test dotnet/CnetMcpServer.Tests/CnetMcpServer.Tests.csproj --no-build --nologo
	$(PYTHON) tests/test_mcp_read_integration.py -v

.PHONY: cnetd
cnetd: $(BIN_DIR)/cnetd
$(BIN_DIR)/cnetd: $(BIN_DIR)/libcnet_capsule_core.so $(ROE_ASI_SRC) tools/cnetd.c src/cnet_domain_route.c \
		src/serve/cnet_capsule_control.c include/cnet_capsule_control.h \
		src/serve/cnet_utterance.c src/serve/cnetd_protocol.c \
		src/memory/cnet_query_alias.c src/memory/cnet_dialog_ctx.c \
		src/cnet_slot_extract.c src/cnet_typed_en.c src/cnet_ffi_convert.c \
		src/cnet_cert_solver.c \
		src/cnet_roe_gold.c src/serve/cnet_showrunner.c \
		src/serve/cnet_marble_live.c src/memory/cnet_md_memory.c \
		src/serve/cnet_mcp_client.c src/serve/cnet_mcp_read_brick.c \
		include/cnet_mcp_read_brick.h include/cnet_mcp_evidence_internal.h \
		src/memory/cnet_chat_lookup.c \
		src/memory/cnet_lookup.c src/cce/cce_campaign_provenance.c \
		src/serve/cnet_c_speak.c src/cce/cce_wordlm.c \
		src/cnet_skill_lane.c src/memory/cnet_capsule_loop.c \
		src/serve/cnet_paragraph.c src/cnet_ood_skill.c \
		src/cnet_held_model.c src/cnet_hemisphere.c src/cnet_brain_mirror.c \
		src/cnet_rlm.c src/serve/cnet_core_serve.c \
		src/memory/cnet_ember.c src/memory/cnet_ember_ckpt.c \
		src/memory/cnet_ember_session.c src/memory/cnet_ember_steer.c \
		include/cnet_core_serve.h include/cnet_probe_shortcircuit.h \
		include/cnet_domain_route.h include/cnet_utterance.h \
		include/cnetd_protocol.h include/cnet_query_alias.h \
		include/cnet_dialog_ctx.h include/cnet_slot_extract.h \
		include/cnet_showrunner.h include/cnet_marble_live.h \
		include/cnet_mcp_client.h include/cnet_chat_lookup.h \
		include/cnet_lookup.h include/cnet_c_speak.h \
		include/cnet_capsule_loop.h include/cnet_skill_lane.h \
		include/cnet_paragraph.h include/cnet_hemisphere.h \
		include/cnet_brain_mirror.h include/cnet_rlm.h include/cnet_ember.h \
		src/cnet_live_miss.c include/cnet_json_internal.h mk/serve.mk
	@mkdir -p $(BIN_DIR) logs
	@pkg-config --exists libcurl
	$(CC) $(ASI_IMPROVE_CFLAGS) -D_DEFAULT_SOURCE -D_GNU_SOURCE -DCNET_HAVE_CURL=1 \
		$$(pkg-config --cflags libcurl) -o $(BIN_DIR)/cnetd \
		$(ROE_ASI_SRC) src/cnet_domain_route.c src/serve/cnet_utterance.c \
		src/serve/cnetd_protocol.c src/serve/cnet_capsule_control.c src/memory/cnet_query_alias.c \
		src/memory/cnet_dialog_ctx.c src/cnet_slot_extract.c \
		src/cnet_typed_en.c src/cnet_ffi_convert.c src/cnet_cert_solver.c \
		src/cnet_roe_gold.c \
		src/serve/cnet_showrunner.c src/serve/cnet_marble_live.c \
		src/memory/cnet_md_memory.c src/serve/cnet_mcp_client.c src/serve/cnet_mcp_read_brick.c \
		src/memory/cnet_chat_lookup.c src/memory/cnet_lookup.c \
		src/cce/cce_campaign_provenance.c src/serve/cnet_c_speak.c \
		src/cce/cce_wordlm.c src/cnet_skill_lane.c \
		src/memory/cnet_capsule_loop.c src/serve/cnet_paragraph.c \
		src/cnet_ood_skill.c src/cnet_held_model.c src/cnet_hemisphere.c \
		src/cnet_brain_mirror.c src/cnet_rlm.c \
		src/serve/cnet_core_serve.c src/cnet_live_miss.c \
		src/memory/cnet_ember.c src/memory/cnet_ember_ckpt.c \
		src/memory/cnet_ember_session.c src/memory/cnet_ember_steer.c \
		tools/cnetd.c $(ROE_ASI_LIBS) $$(pkg-config --libs libcurl) -ldl \
		-L$(BIN_DIR) -lcnet_capsule_core -Wl,-rpath,'$$ORIGIN'
	@echo "cnetd built -> $(BIN_DIR)/cnetd"

.PHONY: cnet_utterance
cnet_utterance: src/serve/cnet_utterance.c include/cnet_utterance.h tools/cnet_utterance_main.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/cnet_utterance \
		src/serve/cnet_utterance.c tools/cnet_utterance_main.c
	@./$(BIN_DIR)/cnet_utterance --test | tee logs/cnet_utterance.log
	@grep -q CNET_UTTERANCE_PASS logs/cnet_utterance.log
	@./$(BIN_DIR)/cnet_utterance --when status --hit 0.857 --da 0.47 --ht 0.53 --ado 0.51 --miss 2 | tee -a logs/cnet_utterance.log
	@echo "CNET_UTTERANCE_OK"

.PHONY: cnet_chat1_coherence
cnet_chat1_coherence: include/cnet_query_alias.h src/memory/cnet_query_alias.c \
		include/cnet_dialog_ctx.h src/memory/cnet_dialog_ctx.c \
		include/cnet_utterance.h src/serve/cnet_utterance.c \
		tests/test_cnet_chat1_coherence.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -Werror -D_POSIX_C_SOURCE=200809L \
		-Iinclude -o $(BIN_DIR)/test_cnet_chat1_coherence \
		src/memory/cnet_query_alias.c src/memory/cnet_dialog_ctx.c src/serve/cnet_utterance.c \
		tests/test_cnet_chat1_coherence.c
	@./$(BIN_DIR)/test_cnet_chat1_coherence | tee logs/cnet_chat1_coherence.log
	@grep -q '^CNET_CHAT1_COHERENCE_PASS ' logs/cnet_chat1_coherence.log

.PHONY: cnet_chat1_certified
cnet_chat1_certified: include/cnet_compete_runtime.h src/compete/cnet_compete_runtime.c \
		tests/test_cnet_chat1_certified.c $(ROUTER) $(SPECIALIST_SRC)
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -ffunction-sections -fdata-sections -Iinclude \
		-o $(BIN_DIR)/test_cnet_chat1_certified \
		src/compete/cnet_compete_runtime.c src/compete/cnet_compete_intent.c \
		src/compete/cnet_compete_capsules.c src/cce/cce_wordlm.c \
		$(CNET_COMPETE_CAPSULE_CORE) $(ROUTER) $(SPECIALIST_SRC) \
		tests/test_cnet_chat1_certified.c \
		-Wl,--gc-sections $(LDFLAGS) $(MCP_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_chat1_certified \
		artifacts/cnet_asi5_v5/intent.wlm \
		artifacts/cnet_asi5_v5/intent.meta \
		artifacts/cnet_asi5_v5/capsules | \
		tee logs/cnet_chat1_certified.log
	@grep -q '^CNET_CHAT1_CERTIFIED_PASS ' logs/cnet_chat1_certified.log

.PHONY: cnet_chat1_fluency
cnet_chat1_fluency: include/cnet_utterance.h src/serve/cnet_utterance.c \
		tests/test_cnet_chat1_fluency.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -Werror -D_POSIX_C_SOURCE=200809L \
		-Iinclude -o $(BIN_DIR)/test_cnet_chat1_fluency \
		src/serve/cnet_utterance.c tests/test_cnet_chat1_fluency.c
	@./$(BIN_DIR)/test_cnet_chat1_fluency | tee logs/cnet_chat1_fluency.log
	@grep -q '^CNET_CHAT1_FLUENCY_PASS ' logs/cnet_chat1_fluency.log

.PHONY: cnet_chat_fluency_v1 cnet_chat1_contract_convo
cnet_chat_fluency_v1: include/cnet_chat_fluency.h src/serve/cnet_chat_fluency.c \
		tests/test_cnet_chat_fluency_v1.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -Werror -D_POSIX_C_SOURCE=200809L \
		-Iinclude -o $(BIN_DIR)/test_cnet_chat_fluency_v1 \
		src/serve/cnet_chat_fluency.c tests/test_cnet_chat_fluency_v1.c
	@./$(BIN_DIR)/test_cnet_chat_fluency_v1 | tee logs/cnet_chat_fluency_v1.log
	@grep -q '^CNET_CHAT_FLUENCY_V1_PASS ' logs/cnet_chat_fluency_v1.log

cnet_chat1_contract_convo: include/cnet_chat_fluency.h src/serve/cnet_chat_fluency.c \
		include/cnet_utterance.h src/serve/cnet_utterance.c \
		include/cnet_compete_runtime.h src/compete/cnet_compete_runtime.c \
		tests/test_cnet_chat1_contract_convo.c $(ROUTER) $(SPECIALIST_SRC)
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -ffunction-sections -fdata-sections -Iinclude \
		-o $(BIN_DIR)/test_cnet_chat1_contract_convo \
		src/compete/cnet_compete_runtime.c src/compete/cnet_compete_intent.c \
		src/compete/cnet_compete_capsules.c src/cce/cce_wordlm.c \
		src/serve/cnet_utterance.c src/serve/cnet_chat_fluency.c \
		$(CNET_COMPETE_CAPSULE_CORE) $(ROUTER) $(SPECIALIST_SRC) \
		tests/test_cnet_chat1_contract_convo.c \
		-Wl,--gc-sections $(LDFLAGS) $(MCP_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_chat1_contract_convo \
		artifacts/cnet_asi5_v5/intent.wlm \
		artifacts/cnet_asi5_v5/intent.meta \
		artifacts/cnet_asi5_v5/capsules | \
		tee logs/cnet_chat1_contract_convo.log
	@grep -q '^CNET_CHAT1_CONTRACT_CONVO_PASS ' \
		logs/cnet_chat1_contract_convo.log

.PHONY: cnet_chat1_dynamic cnet_chat1_dual_probe
cnet_chat1_dynamic: include/cnet_chat_fluency.h src/serve/cnet_chat_fluency.c \
		include/cnet_utterance.h src/serve/cnet_utterance.c \
		tests/test_cnet_chat1_dynamic.c $(ROUTER) $(SPECIALIST_SRC)
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -ffunction-sections -fdata-sections -Iinclude \
		-o $(BIN_DIR)/test_cnet_chat1_dynamic \
		src/compete/cnet_compete_runtime.c src/compete/cnet_compete_intent.c \
		src/compete/cnet_compete_capsules.c src/cce/cce_wordlm.c \
		src/serve/cnet_utterance.c src/serve/cnet_chat_fluency.c \
		$(CNET_COMPETE_CAPSULE_CORE) $(ROUTER) $(SPECIALIST_SRC) \
		tests/test_cnet_chat1_dynamic.c \
		-Wl,--gc-sections $(LDFLAGS) $(MCP_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_chat1_dynamic \
		artifacts/cnet_asi5_v5/intent.wlm \
		artifacts/cnet_asi5_v5/intent.meta \
		artifacts/cnet_asi5_v5/capsules | \
		tee logs/cnet_chat1_dynamic.log
	@grep -q '^CNET_CHAT1_DYNAMIC_PASS ' logs/cnet_chat1_dynamic.log

cnet_chat1_dual_probe: tools/cnet_chat1_dual_probe.c include/cnet_chat_fluency.h \
		src/serve/cnet_chat_fluency.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -DCNET_HAVE_CURL=1 \
		-DCNET_COMPETE_SUITE_DATA_HEADER=\"cnet_compete_suite_data_v5.h\" \
		-ffunction-sections -fdata-sections -Iinclude \
		-o $(BIN_DIR)/cnet_chat1_dual_probe \
		tools/cnet_chat1_dual_probe.c src/compete/cnet_compete_runtime.c \
		src/compete/cnet_compete_intent.c src/compete/cnet_compete_capsules.c \
		src/cce/cce_wordlm.c src/serve/cnet_utterance.c src/serve/cnet_chat_fluency.c \
		src/compete/cnet_compete_eval.c src/compete/cnet_compete.c \
		src/cce/cce_campaign_provenance.c \
		$(CNET_COMPETE_CAPSULE_CORE) $(ROUTER) $(SPECIALIST_SRC) \
		-Wl,--gc-sections $(LDFLAGS) $(MCP_LDFLAGS) -pthread
	@./$(BIN_DIR)/cnet_chat1_dual_probe \
		artifacts/cnet_asi5_v5/intent.wlm \
		artifacts/cnet_asi5_v5/intent.meta \
		artifacts/cnet_asi5_v5/capsules | \
		tee logs/cnet_chat1_dual_probe.log
	@grep -q '^CNET_CHAT1_DUAL_PROBE_DONE ' logs/cnet_chat1_dual_probe.log

CNET_CHAT1_STATE_ROOT ?= /home/marble/.local/state/cnet/cnet_asi_chat1
.PHONY: cnet_chat1_fixture cnet_chat1_independence cnet_chat1_compete
cnet_chat1_fixture: include/cnet_chat1.h tools/cnet_chat1_fixture.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -Werror -Iinclude \
		-o $(BIN_DIR)/cnet_chat1_fixture tools/cnet_chat1_fixture.c

cnet_chat1_independence: include/cnet_chat1.h tools/cnet_chat1_independence.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -Werror -Iinclude \
		-o $(BIN_DIR)/cnet_chat1_independence \
		tools/cnet_chat1_independence.c
	@test -f benchmarks/cnet_asi_chat1/heldout.tsv
	@./$(BIN_DIR)/cnet_chat1_independence \
		benchmarks/cnet_asi_chat1/heldout.tsv \
		benchmarks/cnet_asi5_v5/heldout.tsv | \
		tee logs/cnet_chat1_independence.log
	@grep -q '^CNET_CHAT1_INDEPENDENCE_PASS ' \
		logs/cnet_chat1_independence.log

cnet_chat1_compete: cnet_chat1_independence include/cnet_chat_fluency.h \
		src/serve/cnet_chat_fluency.c tools/cnet_chat1_compete.c
	@mkdir -p $(BIN_DIR) logs $(CNET_CHAT1_STATE_ROOT)
	$(CC) $(CFLAGS) -Werror \
		-DCNET_COMPETE_SUITE_DATA_HEADER=\"cnet_compete_suite_data_v5.h\" \
		-ffunction-sections -fdata-sections -Iinclude \
		-o $(BIN_DIR)/cnet_chat1_compete \
		tools/cnet_chat1_compete.c src/compete/cnet_compete_runtime.c \
		src/compete/cnet_compete_intent.c src/compete/cnet_compete_capsules.c \
		src/cce/cce_wordlm.c src/serve/cnet_utterance.c src/serve/cnet_chat_fluency.c \
		src/compete/cnet_compete_eval.c src/compete/cnet_compete.c \
		src/cce/cce_campaign_provenance.c \
		$(CNET_COMPETE_CAPSULE_CORE) $(ROUTER) $(SPECIALIST_SRC) \
		-Wl,--gc-sections $(LDFLAGS) $(MCP_LDFLAGS) -pthread -lcurl
	@./$(BIN_DIR)/cnet_chat1_compete \
		artifacts/cnet_asi5_v5/intent.wlm \
		artifacts/cnet_asi5_v5/intent.meta \
		artifacts/cnet_asi5_v5/capsules | \
		tee logs/cnet_chat1_compete.log
	@cp logs/cnet_chat1_compete.log \
		$(CNET_CHAT1_STATE_ROOT)/cnet_chat1_compete.log
	@grep -q '^CNET_CHAT_COMPETE_PASS ' logs/cnet_chat1_compete.log

.PHONY: cnet_lookup_capsule
cnet_lookup_capsule: include/cnet_lookup.h src/memory/cnet_lookup.c \
		src/cce/cce_campaign_provenance.c \
		tests/test_cnet_lookup_capsule.c
	@mkdir -p $(BIN_DIR) logs
	@pkg-config --exists libcurl
	$(CC) $(CFLAGS) -Werror -Iinclude $$(pkg-config --cflags libcurl) \
		-o $(BIN_DIR)/test_cnet_lookup_capsule \
		src/memory/cnet_lookup.c src/cce/cce_campaign_provenance.c \
		tests/test_cnet_lookup_capsule.c $(LDFLAGS) \
		$$(pkg-config --libs libcurl)
	@./$(BIN_DIR)/test_cnet_lookup_capsule | tee logs/cnet_lookup_capsule.log
	@grep -q '^CNET_LOOKUP_CAPSULE_PASS ' logs/cnet_lookup_capsule.log

.PHONY: cnet_chat_lookup
cnet_chat_lookup: include/cnet_chat_lookup.h src/memory/cnet_chat_lookup.c \
		include/cnet_lookup.h src/memory/cnet_lookup.c \
		src/cce/cce_campaign_provenance.c \
		tests/test_cnet_chat_lookup.c
	@mkdir -p $(BIN_DIR) logs
	@pkg-config --exists libcurl
	$(CC) $(CFLAGS) -Werror -Iinclude $$(pkg-config --cflags libcurl) \
		-o $(BIN_DIR)/test_cnet_chat_lookup \
		src/memory/cnet_chat_lookup.c src/memory/cnet_lookup.c \
		src/cce/cce_campaign_provenance.c \
		tests/test_cnet_chat_lookup.c $(LDFLAGS) \
		$$(pkg-config --libs libcurl)
	@./$(BIN_DIR)/test_cnet_chat_lookup | tee logs/cnet_chat_lookup.log
	@grep -q '^CNET_CHAT_LOOKUP_PASS ' logs/cnet_chat_lookup.log

.PHONY: cnet_lookup
cnet_lookup: include/cnet_lookup.h src/memory/cnet_lookup.c \
		src/cce/cce_campaign_provenance.c tools/cnet_lookup.c
	@mkdir -p $(BIN_DIR)
	@pkg-config --exists libcurl
	$(CC) $(CFLAGS) -Werror -Iinclude $$(pkg-config --cflags libcurl) \
		-o $(BIN_DIR)/cnet_lookup \
		src/memory/cnet_lookup.c src/cce/cce_campaign_provenance.c \
		tools/cnet_lookup.c $(LDFLAGS) $$(pkg-config --libs libcurl)

.PHONY: cnetd-run
cnetd-run: cnetd query_dialog
	@pkill -x cnetd 2>/dev/null || true
	@sleep 0.2
	@if [ -f $(HOME)/.local/share/cnet-minimal/cnet-minimal.env ]; then set -a; . $(HOME)/.local/share/cnet-minimal/cnet-minimal.env; set +a; fi
	@$(BIN_DIR)/cnetd >logs/cnetd.log 2>&1 & echo $$! > logs/cnetd.pid
	@sleep 0.4
	@chmod +x scripts/cnet_sock_ask.sh
	@scripts/cnet_sock_ask.sh "who are you" | tee logs/cnetd_ask.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask.log
	@# Milestone A: conversational soul paraphrase â†’ LOCAL
	@scripts/cnet_sock_ask.sh "Introduce yourself" | tee logs/cnetd_ask_a1.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_a1.log
	@cat logs/cnetd_ask_a1.log >> logs/cnetd_ask.log
	@scripts/cnet_sock_ask.sh "who am i talking to" | tee logs/cnetd_ask_a2.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_a2.log
	@cat logs/cnetd_ask_a2.log >> logs/cnetd_ask.log
	@# Milestone B: entity bind then anaphora status
	@scripts/cnet_sock_ask.sh "cnet-marble status" | tee logs/cnetd_ask_b1.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_b1.log
	@cat logs/cnetd_ask_b1.log >> logs/cnetd_ask.log
	@scripts/cnet_sock_ask.sh "show me its status" | tee logs/cnetd_ask_b2.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_b2.log
	@grep -q "DIALOG_HIT 1\|\"dialog_hit\":true" logs/cnetd_ask_b2.log
	@cat logs/cnetd_ask_b2.log >> logs/cnetd_ask.log
	@# Milestone C: pack-local ops slots
	@scripts/cnet_sock_ask.sh "is cnet-web active" | tee logs/cnetd_ask_c1.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_c1.log
	@grep -q "SLOT_HIT 1\|\"slot_hit\":true" logs/cnetd_ask_c1.log
	@cat logs/cnetd_ask_c1.log >> logs/cnetd_ask.log
	@scripts/cnet_sock_ask.sh "restart cnetd" | tee logs/cnetd_ask_c2.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_c2.log
	@grep -q "SLOT_HIT 1\|\"slot_hit\":true" logs/cnetd_ask_c2.log
	@cat logs/cnetd_ask_c2.log >> logs/cnetd_ask.log
	@scripts/cnet_sock_ask.sh "is cnet-marble running" | tee logs/cnetd_ask_c3.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_c3.log
	@grep -q "ops_cnet_marble\|SLOT_HIT 1\|\"slot_hit\":true" logs/cnetd_ask_c3.log
	@cat logs/cnetd_ask_c3.log >> logs/cnetd_ask.log
	@# probes remain non-LOCAL / short-circuit
	@scripts/cnet_sock_ask.sh "autonomous cycle probe novel fact beta-nine" | tee logs/cnetd_ask_probe.log
	@grep -q "SHORTCIRCUIT 1\|shortcircuit.:true" logs/cnetd_ask_probe.log
	@cat logs/cnetd_ask_probe.log >> logs/cnetd_ask.log
	@echo "CNETD_OK"
