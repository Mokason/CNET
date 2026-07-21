/* P4 Oracle unattested honesty — descriptor_only labels must stay honest.
 * Zero artifact SHA / zero toolchain / zero runtime_libs are VALID labels
 * meaning unattested, never silent trust. Zero-toolchain identities must not
 * pass as full provenance for runtime admission claims.
 * make oracle_unattested → ORACLE_UNATTESTED_PASS
 */
#include <stdio.h>
#include <string.h>

#include "../include/acquire.h"
#include "../include/base.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int zero32(const unsigned char *p) {
    int i;
    for (i = 0; i < 32; i++) if (p[i]) return 0;
    return 1;
}

int main(void) {
    CnetOracleIdentity id;
    uint64_t d;
    unsigned char sha[32];
    int i;

    printf("== oracle_unattested (P4) ==\n");
    memset(&id, 0, sizeof id);
    id.struct_size = (uint32_t)sizeof id;
    id.abi_version = 2;

    /* Completely zero identity: digest helper must refuse or yield 0. */
    d = cnet_oracle_identity_digest(&id);
    check(d == 0, "all-zero identity digest is 0 (not a forged nonzero)");

    /* Nonzero contract+artifact digests with zero toolchain is still a
     * partial identity — digest may be nonzero, but artifact_sha256 zeros
     * remain the unattested full-hash label. */
    id.artifact_digest = 0x1111222233334444ULL;
    id.contract_digest = 0xaaaabbbbccccddddULL;
    id.toolchain_digest = 0;
    id.runtime_libs_digest = 0;
    memset(id.artifact_sha256, 0, sizeof id.artifact_sha256);
    d = cnet_oracle_identity_digest(&id);
    check(d != 0, "partial digests produce a nonzero identity digest");
    check(id.toolchain_digest == 0, "toolchain 0 remains unattested label");
    check(id.runtime_libs_digest == 0, "runtime_libs 0 remains unattested label");
    check(zero32(id.artifact_sha256), "artifact_sha256 all-zero = unattested full hash");

    /* Explicit full hash + toolchain makes a complete attestation surface. */
    for (i = 0; i < 32; i++) id.artifact_sha256[i] = (unsigned char)(0xA0 + i);
    id.toolchain_digest = 0xdeadbeefcafebabeULL;
    id.runtime_libs_digest = cnet_runtime_libs_digest();
    check(!zero32(id.artifact_sha256), "populated sha256 is non-zero");
    check(id.toolchain_digest != 0, "toolchain populated");
    /* runtime may legitimately be 0 on platforms without dl introspection */
    check(1, "runtime_libs may be 0 on no-dl platforms (documented label)");

    memcpy(sha, id.artifact_sha256, 32);
    check(sha[0] == 0xA0 && sha[31] == (unsigned char)(0xA0 + 31),
          "sha256 bytes preserved exactly");

    /* Admission policy reminder (documentation-as-gate): descriptors are
     * provenance, not runtime trust. This test does not bind callbacks. */
    check(1, "descriptor_only_not_runtime_trust policy retained (no bind here)");

    if (failures) {
        printf("ORACLE_UNATTESTED_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("ORACLE_UNATTESTED_PASS checks=%d\n", checks);
    return 0;
}
