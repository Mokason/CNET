.PHONY: roe_process_security
roe_process_security: tests/test_roe_process_security.c \
		src/roe/cnet_roe_doc.c src/roe/cnet_roe_ocr.c \
		src/roe/cnet_roe_goal.c src/roe/cnet_roe_table.c $(ROE_ASI_SRC)
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_roe_process_security \
		$(ROE_ASI_SRC) src/roe/cnet_roe_goal.c src/roe/cnet_roe_table.c \
		src/roe/cnet_roe_doc.c src/roe/cnet_roe_ocr.c \
		tests/test_roe_process_security.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/test_roe_process_security > logs/roe_process_security.log 2>&1
	@grep -q "ROE_PROCESS_SECURITY_PASS" logs/roe_process_security.log
	@grep "ROE_PROCESS_SECURITY_PASS" logs/roe_process_security.log
