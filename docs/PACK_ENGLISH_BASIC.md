# Elementary English pack

The tracked [pack](../packs/pack_english_basic/) supplies curated vocabulary
and grammar responses for the ROE front door. The runtime copy normally lives
under `artifacts/roe_daily_packs/pack_english_basic`; routes also involve
[domain_routes.tsv](../config/domain_routes.tsv).

This is finite pack coverage, not a general language-quality benchmark.
Teacher drafts and missed queries do not become certified responses merely
by being added to a curriculum.

Use `make roe_daily_packs` and `make domain_route` for the pack/routing gates.
Installation is a separate operator action: inspect the deployed root, preserve
local additions, validate the pack and coordinate its readers before replacing
a runtime copy. Do not blindly copy an old tracked directory over live state.

Example queries include `a or an`, `past tense of go` and
`what is a noun`. Check returned source and skill identity on the selected
private socket; a successful match covers that pack response only.
See [gold review](GOLD_CURRICULUM_HARVEST.md) and [operations](CNET_MARBLE_24_7.md).
