CC ?= cc
ROOT := ../..
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -I$(ROOT)/include
OUT ?= /tmp/cnet-controller-build
.PHONY: test
test: $(OUT)/test_fixture
	$(OUT)/test_fixture
$(OUT)/test_fixture: fixture.c fixture.h test_fixture.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) test_fixture.c fixture.c -o $@
