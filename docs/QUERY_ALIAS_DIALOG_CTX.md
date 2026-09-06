# Aliases, slots and dialog context

These helpers prepare queries for existing routes. They do not create new
certified capabilities or authorize the operations named in a rewritten string.

| Helper | Contract |
| --- | --- |
| [Query alias](../include/cnet_query_alias.h) | Normalize and map aliases onto known patterns |
| [Slot extraction](../include/cnet_slot_extract.h) | Recognize bounded operational grammars |
| [Dialog context](../include/cnet_dialog_ctx.h) | Fill follow-up references from recorded entities/actions |

The daemon checks probes against the original query before preparation.
Its ordinary preparation sequence is alias/normalization, operational slots,
dialog resolution, then pack matching. Empty or unsuitable context must not
invent an entity. Dialog state is process-local to the serial daemon, not a
per-user secure multi-tenant conversation store.

```sh
make query_alias query_dialog slot_extract cnetd_protocol_boundary
```

`Introduce yourself` can map to an identity pattern; a contextual
`show me its status` can reuse a preceding service name. A string such as
`systemctl --user restart ...` in prepared text is not permission to execute
it. Query access itself can invoke other mutating daemon commands, so protect
the socket and web credentials as described in [CNETD.md](CNETD.md).

[config/query_aliases.tsv](../config/query_aliases.tsv) is operational input.
Changing it or copying a daemon binary into an installed tree is deployment,
not a harmless documentation example.
