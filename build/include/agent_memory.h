/*
 * include/agent_memory.h
 * Minimal agent memory + MCP compatibility interface for the build tool demo.
 */

#ifndef CNET_AGENT_MEMORY_H
#define CNET_AGENT_MEMORY_H

#include <stddef.h>

void sanitize_for_filename(const char *input, char *out, size_t cap);
void agent_record_thought(const char *thought);
int port_contract_mcp_file_write(const char *path, const char *content, char *result_out, size_t cap);

#endif // CNET_AGENT_MEMORY_H
