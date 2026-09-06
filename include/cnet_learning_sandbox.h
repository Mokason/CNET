#ifndef CNET_LEARNING_SANDBOX_H
#define CNET_LEARNING_SANDBOX_H
#include <stddef.h>

/* Irreversible Linux/x86-64 guard for a fixed, trusted native worker. Call from
 * single-threaded startup BEFORE parsing source/evidence. Returns 0 sealed,
 * -1 refused; on refusal exit immediately (restrictions may already apply).
 * output_root: absolute, existing, empty owner-private directory with trusted
 * ancestors; the only filesystem write/create/rename/link allowance. No chmod,
 * symlink, device, socket, exec, new thread or external process authority.
 * Reads are NOT confidential isolation. Kernel/runtime/installed code and the
 * trusted parent are the trust base, not arbitrary hostile native programs.
 *
 * cpu_seconds: 1..120; address_bytes: 16MiB..2GiB; file_bytes: 1..16MiB.
 * Per-file size and address space are hard limits, NOT total disk/RSS quotas.
 * Std input must be a read-only regular file/pipe or /dev/null. Std output and
 * error must be writable pipes or /dev/null, never regular files or sockets.
 * All FDs >=3 close. The parent creates an empty 0700 output root and supplies
 * these streams and starts a pinned executable with a cleared environment
 * (loader initialization precedes this function). Guard clears environment,
 * sets umask077 and parent-death SIGKILL. Parent still
 * enforces a suspend-inclusive BOOTTIME deadline, kills and REAPS on refusal,
 * timeout or cancellation, and validates/freezes output only after reaping.
 * expected_parent: original launcher's PID, captured by the launcher BEFORE
 * spawning this worker. Must be >1 and match before/after setting parent-death
 * protection. Refuses adoption by a subreaper before sealing.
 * Do not use CNET-owned answers as labels or treat worker output as approval. */
int cnet_learning_sandbox_enter_for_parent(const char *output_root,unsigned cpu_seconds,
    size_t address_bytes,size_t file_bytes,int expected_parent);
/* Compatibility only: binds the current parent at entry, and cannot detect
 * earlier launcher death/reparenting. Unattended launchers use _for_parent. */
int cnet_learning_sandbox_enter(const char *output_root,unsigned cpu_seconds,
                               size_t address_bytes,size_t file_bytes);
#endif
