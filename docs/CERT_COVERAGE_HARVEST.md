# Pack coverage harvesting

[scripts/cert_coverage_harvest.sh](../scripts/cert_coverage_harvest.sh) is a
legacy pack-harvest workflow for seeding patterns and checking probe refusal.
It currently calls the removed `tools/roe_daily_packs_seed.py`; therefore
`make cert_coverage_harvest` is blocked, not a passing installation recipe.
Do not restore the obsolete Python product path to hide that migration gap.

Separate native pack and route gates remain available:

```sh
make roe_daily_packs domain_route
```

These do not replace the missing full harvest execution. They are pack/routing
fixtures, not proof of newly learned unseen domains. Use a private worktree:
pack seeding and harvest tools write artifact/gold files.
A miss, an abstention string or a curriculum hint cannot certify itself.
Keep dynamic route files and compiled defaults consistent; the
[domain router](DOMAIN_ROUTE.md) does not install an MTK cartridge.

The legacy ROE pack label CERT must not be equated with arbitrary neural
generalization or CNU1 portability. Independent evidence and the relevant gate
remain necessary. For current typed acquisition, follow
[capsule core](CAPSULE_CORE.md); for ROE gold handling, follow
[curriculum harvest](GOLD_CURRICULUM_HARVEST.md).
