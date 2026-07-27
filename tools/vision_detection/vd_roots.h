/* Canonical source-identity roots for the pre-registered protocols.
 *
 * Each root is sha256 over the sorted "value\n" lines of the canonical list:
 * image IDs for an id_root, source-JPEG content hashes for a content_root.
 * They are derived from the official VOC2007 archives under the frozen
 * selection rule and pinned here so that source identity is checked against a
 * committed constant rather than against a cache's own manifest.
 *
 * An empty string means "not yet pinned"; the bench refuses to score a
 * protocol whose roots are unpinned, so a bootstrap build cannot produce a
 * verdict.
 */
#ifndef VD_ROOTS_H
#define VD_ROOTS_H

#define VD_ROOT_ID_TRAIN       "714b3061a4c19e68413db09b263237540308ffec240de9e6051548eb2f0f9758"
#define VD_ROOT_ID_VAL         "60d20c5c9b8ddfd6483ad5fbb8d1d47b9655fd8240cffe9ede22cc7d5fc106d7"
#define VD_ROOT_ID_TEST        "a58e79541baca36d867e3c985dd0268c8a3cb78dc96a523030d2f0471d362264"
#define VD_ROOT_CONTENT_TRAIN  "fc168bb629de965d62cd1117aabe77b20f5dd0e4c52f9eb42cec0d117320096f"
#define VD_ROOT_CONTENT_VAL    "2dd568091adcc02ae30a9463744121bbc091a0608707020e194b894d2c956273"
#define VD_ROOT_CONTENT_TEST   "0a0e1ce705abe301016119b37c5e2e44a7041a817663a10c60224f21a64baffa"

#endif
