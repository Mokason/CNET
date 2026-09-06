#ifndef CNET_WORKER_SANDBOX_H
#define CNET_WORKER_SANDBOX_H
/* Linux Landlock ABI >=3 required, no silent fallback. Call filesystem_seal
 * before HIP creates threads; /dev/kfd, /dev/dri, /dev/null and the optional
 * owner-private job scratch directory retain write access. COMGR needs scratch.
 * Close inherited descriptors first: preexisting FDs are not retroactively
 * restricted. Then warm the GPU, call offline_seal and process_seal (TSYNC).
 * Trusted compiled worker/HIP runtime, not isolation from a hostile GPU driver. */
int worker_filesystem_seal(const char *scratch);
int worker_process_seal(void);
#endif
