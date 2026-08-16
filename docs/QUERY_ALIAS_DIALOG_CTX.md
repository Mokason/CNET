# Conversational aliases + dialog context (Milestones A+B)

## Law

- **No soft CERT.** Aliases only rewrite onto already-sealed skill patterns.
- **Probe short-circuit** always matches the **original** user string (never alias-smoothed).
- **Anaphora** fills slots from `last_entities` only; empty ctx → no rewrite.
- **Never auto-CERT** from Teacher / cosine / freeform chat.

## Milestone A — `cnet_query_alias`

| Piece | Path |
|-------|------|
| API | `include/cnet_query_alias.h` · `src/cnet_query_alias.c` |
| TSV | `config/query_aliases.tsv` |
| Gate | `make query_alias` / `make query_dialog` → `QUERY_ALIAS_PASS` |

Flow:

```text
user text → normalize (contractions, case, strip ?!) → longest alias rewrite
       → CERT skill pattern match (unchanged binary contains_ci)
```

Examples:

| Input | Prepared |
|-------|----------|
| `Introduce yourself` | `who are you` |
| `who am i talking to` | `who are you` |
| `What's your name?` | `what is your name` → alias → `who are you` |

## Milestone B — `cnet_dialog_ctx`

| Piece | Path |
|-------|------|
| API | `include/cnet_dialog_ctx.h` · `src/cnet_dialog_ctx.c` |
| Host | process-local in `cnetd` (single-user warm waist) |
| Gate | `DIALOG_CTX_PASS` inside `make query_dialog` |

After each non-probe turn, cnetd stores `last_skill`, `last_pack`, entities
(`cnet-marble`, `cnet-web`, `*.service`, …), and action.

Follow-ups:

| Prior | Follow-up | Rewritten |
|-------|-----------|-----------|
| `cnet-marble status` | `show me its status` | `cnet-marble status` |
| same | `restart it` | `systemctl --user restart cnet-marble` |
| same | `do it again` | last canonical |

Rewritten queries still must hit sealed CERT patterns (e.g. `systemctl --user`).

## Milestone C — `cnet_slot_extract` (ops-only)

| Piece | Path |
|-------|------|
| API | `include/cnet_slot_extract.h` · `src/cnet_slot_extract.c` |
| Gate | `SLOT_EXTRACT_PASS` inside `make query_dialog` / `make slot_extract` |

Fixed grammars (normalized text):

| Input shape | Sealed rewrite |
|-------------|----------------|
| `is <unit> active\|running` | `systemctl --user status <unit>` |
| `status of <unit>` | same |
| `<unit> status` | same |
| `restart\|start\|stop <unit>` | `systemctl --user <verb> <unit>` |
| marble-family status | `cnet-marble status` (special sealed skill) |

Pronouns / stopwords are refused as units. Pipeline order in cnetd:

```text
probe(raw) → alias/normalize → slot_extract_ops → dialog anaphora → CERT match
```

## Wire-up

- `tools/cnetd.c` — prepare + dialog before `roe_turn`; reply fields `alias_hit`, `dialog_hit`, `prepared`
- `tools/roe_front_door.c` — same prepare path; prints `prepared=` / `alias_hit=`
- Package copies `config/query_aliases.tsv`

## Commands

```bash
cd /home/marble/AI/CNET
make query_dialog
make cnetd
# full gate incl. A+B sock asks:
make cnetd-run
# or against live unit after deploy:
systemctl --user restart cnetd
cnet-sock-ask "Introduce yourself"
cnet-sock-ask "cnet-marble status"
cnet-sock-ask "show me its status"
```

## Deploy

```bash
cp -a bin/cnetd bin/roe_front_door ~/.local/share/cnet-minimal/current/bin/
cp -a config/query_aliases.tsv config/domain_routes.tsv \
  ~/.local/share/cnet-minimal/current/config/
systemctl --user restart cnetd
```
