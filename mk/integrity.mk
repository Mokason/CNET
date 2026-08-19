# mk/integrity.mk -- the gates that keep the build honest.
#
# Each one exists because the thing it checks silently broke: curl_guard for a
# preprocessor guard that killed cce_dll, orphan_tests/orphan_tools for source
# files with no rule, platform_sweep for POSIX calls written without the shim,
# layering_guard for new core back-edges, makefile_budget for this file growing.


# --- the three gates that keep the build honest -------------------------------

# Every tests/*.c must have a rule. See tests/orphan_tests.sh.
.PHONY: orphan_tests
orphan_tests:
	@mkdir -p logs
	@sh tests/orphan_tests.sh 2>&1 | tee logs/orphan_tests.log
	@grep -q '^ORPHAN_TESTS_PASS' logs/orphan_tests.log

# CNET_HAVE_CURL is always defined, so #ifdef is always true. See
# tests/curl_guard.sh -- this is the exact break that killed cce_dll.
.PHONY: curl_guard
curl_guard:
	@mkdir -p logs
	@sh tests/curl_guard.sh 2>&1 | tee logs/curl_guard.log
	@grep -q '^CURL_GUARD_PASS' logs/curl_guard.log

# Every src/*.c must compile on this platform. Catches a POSIX call written
# without the shim at the file that introduced it, not at whichever build first
# happens to link it.
# Cached by sorted list of src/**/*.c mtimes+sizes. Force with PLATFORM_SWEEP_FORCE=1.
.PHONY: platform_sweep
platform_sweep:
	@mkdir -p logs
	@stamp=logs/.platform_sweep.stamp; \
	hash=$$(find src -name '*.c' -printf '%p %T@ %s\n' 2>/dev/null | sort | sha256sum | awk '{print $$1}'); \
	if [ -z "$$hash" ]; then hash=$$(find src -name '*.c' -exec stat -c '%n %Y %s' {} + 2>/dev/null | sort | sha256sum | awk '{print $$1}'); fi; \
	if [ "$${PLATFORM_SWEEP_FORCE:-0}" != "1" ] && [ -f "$$stamp" ] && [ "$$(cat $$stamp 2>/dev/null)" = "$$hash" ] \
		&& grep -q PLATFORM_SWEEP_PASS logs/platform_sweep.log 2>/dev/null; then \
		echo PLATFORM_SWEEP_PASS cached | tee logs/platform_sweep.log; exit 0; \
	fi; \
	rm -f logs/platform_sweep.log; \
	bad=0; for f in $$(find src -name '*.c' | sort); do \
		extra=; \
		case $$f in src/runtime_identity.c) extra=-D_GNU_SOURCE;; esac; \
		$(CC) -fsyntax-only $$extra $(CFLAGS) -Iinclude -Iinclude/cce -Isrc/cce $$f \
			2>> logs/platform_sweep.log || { echo "PLATFORM_SWEEP_FAIL $$f" >> logs/platform_sweep.log; bad=1; }; \
	done; \
	if [ $$bad -ne 0 ]; then grep '^PLATFORM_SWEEP_FAIL' logs/platform_sweep.log; exit 1; fi; \
	echo "$$hash" > "$$stamp"; \
	echo PLATFORM_SWEEP_PASS | tee -a logs/platform_sweep.log

# The optional-libcurl degraded path, compiled the way a box without libcurl
# compiles it. This path was designed for and unreachable for months.
.PHONY: residual_http_nocurl
residual_http_nocurl:
	@mkdir -p logs
	$(CC) -fsyntax-only $(CFLAGS) -UCNET_HAVE_CURL -DCNET_HAVE_CURL=0 -Iinclude -Iinclude/cce \
		src/residual_http.c src/memory/cnet_lookup.c src/cce/cce_safetensors.c src/cnet_held_model.c src/roe/cnet_roe_net.c
	@echo RESIDUAL_HTTP_NOCURL_PASS | tee logs/residual_http_nocurl.log

# One name for build-integrity + tier membership honesty.
.PHONY: build_integrity verify_tier_sync
verify_tier_sync:
	@mkdir -p logs
	@sh tests/verify_tier_sync.sh 2>&1 | tee logs/verify_tier_sync.log
	@grep -q '^VERIFY_TIER_SYNC_PASS' logs/verify_tier_sync.log

build_integrity: curl_guard orphan_tests orphan_tools platform_sweep residual_http_nocurl layering_guard makefile_budget verify_tier_sync
	@echo BUILD_INTEGRITY_PASS

# The certified core must not grow new dependencies on the layers above it.
# See tests/layering_guard.sh; baseline in tests/layering_baseline.txt.
.PHONY: layering_guard
layering_guard:
	@mkdir -p logs
	@sh tests/layering_guard.sh 2>&1 | tee logs/layering_guard.log
	@grep -q '^LAYERING_GUARD_PASS' logs/layering_guard.log

# The root Makefile may shrink, never grow. See tests/makefile_budget.sh and
# mk/README.md -- the full split is staged, and this keeps the staging honest.
.PHONY: makefile_budget
makefile_budget:
	@mkdir -p logs
	@sh tests/makefile_budget.sh 2>&1 | tee logs/makefile_budget.log
	@grep -q '^MAKEFILE_BUDGET_PASS' logs/makefile_budget.log
