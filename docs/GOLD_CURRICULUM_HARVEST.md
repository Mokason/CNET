# Gold evidence and curriculum hints

ROE's [harvester](../tools/roe_gold_curriculum_harvest.py) prepares reviewed
evidence for the pack gardener. Gold files, curriculum rows and an accepted
pack skill are different states.

| Artifact | Authority |
| --- | --- |
| Gold file | External review evidence; not automatic certification |
| Curriculum JSONL | Acquisition hint; `auto_cert=false` |
| Installed pack skill | Must pass the applicable promotion path |

Run `make gold_curriculum_harvest` for the focused fixture gate.
[config/promote_blocklist.txt](../config/promote_blocklist.txt) is a policy input:
retain its substring, answer-prefix and regular-expression rules. It is not
obsolete prose.

The active native evolution entry is [roe_evolve_tick.c](../tools/roe_evolve_tick.c).
Do not copy old commands for removed Python evolution scripts. Before operating
a real gardener, inspect its effective paths and review policy.
Neither repeated CNET output nor an ABSTAIN answer supplies a new independent
label. See [teacher boundaries](TEACH_PATH.md) and [operations](CNET_MARBLE_24_7.md).
