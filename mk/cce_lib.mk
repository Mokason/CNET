# mk/cce_lib.mk -- libcce is the real CCE build product.
#
# Until this file, every CCE test recipe recompiled ~50 translation units from
# source. That is not a test strategy; it is a missing library boundary.
#
# Product:
#   bin/libcce.a     static archive (default link for tools/tests)
#   bin/libcce.so    shared object (ELF); cce.dll is the Windows/PInvoke name
#                    built from the same objects with -DCCE_BUILD_DLL
#
# Included at EOF after $(CCE) is fully defined. LIBCCE is the prereq+link
# token recipes should use instead of listing $(CCE) sources again.

AR ?= ar

CCE_OBJDIR := build/cce
# $(CCE) may already contain $(CCE_CUDA_OBJ) (.o) and only .c sources besides.
CCE_C_SRCS := $(filter %.c,$(CCE))
CCE_C_OBJS := $(patsubst src/cce/%.c,$(CCE_OBJDIR)/%.o,$(CCE_C_SRCS))
# Extra .o already compiled (CUDA) that are in $(CCE) but not .c
CCE_PREBUILT_OBJS := $(filter %.o,$(CCE))

LIBCCE := $(BIN_DIR)/libcce.a
LIBCCE_SO := $(BIN_DIR)/libcce.so

.PHONY: libcce libcce_shared cce_lib cce_dll

# Per-TU compile once. -fPIC so the same objects feed the shared lib.
$(CCE_OBJDIR)/%.o: src/cce/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -fPIC -c -o $@ $<

$(LIBCCE): $(CCE_C_OBJS) $(CCE_PREBUILT_OBJS)
	@mkdir -p $(BIN_DIR)
	$(AR) rcs $@ $(CCE_C_OBJS) $(CCE_PREBUILT_OBJS)
	@echo "Built $@ ($(words $(CCE_C_OBJS)) objects)"

$(LIBCCE_SO): $(CCE_C_OBJS) $(CCE_PREBUILT_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) -shared -DCCE_BUILD_DLL $(CFLAGS) $(CUDA_CFLAGS) -o $@ \
		$(CCE_C_OBJS) $(CCE_PREBUILT_OBJS) \
		$(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	@echo "Built $@ (shared CCE)"

libcce: $(LIBCCE)
libcce_shared: $(LIBCCE_SO)
cce_lib: $(LIBCCE) $(LIBCCE_SO)

# cce.dll: real shared CCE artifact at repo root for P/Invoke hosts.
# Built from the same objects — not a second full source compile.
cce_dll: $(CCE_C_OBJS) $(CCE_PREBUILT_OBJS)
	$(CC) -shared -DCCE_BUILD_DLL $(CFLAGS) $(CUDA_CFLAGS) -o cce.dll \
		$(CCE_C_OBJS) $(CCE_PREBUILT_OBJS) \
		$(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	@cp -f cce.dll $(BIN_DIR)/cce.dll 2>/dev/null || true
	@echo "Built cce.dll (shared CCE; same objects as libcce)"

# Convenience: anything that still expands $(CCE) as a bare source list on the
# link line can switch to $(LIBCCE) for prereq + link. LDFLAGS stay as before.
