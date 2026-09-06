# Instrumented inventory

The [self-model API](../include/cnet_roe_self.h) exposes an inventory snapshot,
goal HAVE/MISS probes, skill health, optional document coverage and pack export.
Its implementation is [cnet_roe_self.c](../src/roe/cnet_roe_self.c).

```sh
make roe_asi_self roe_asi_self_cli roe_soul_pack
```

Snapshot/export operations write artifacts such as `SELF.abi`,
`self_report.json`, a manifest and optional skill-tree/document data.
Use a private output for manual experiments; inspect the CLI options before
writing to an existing catalog.

Gate-evidence files and explicit doctrine roots influence the tree.
A discovered PASS marker is historical file content, not proof of a fresh
run or authenticated achievement. Preserve its source and benchmark scope.
The inventory cannot certify its own statements or supply independent labels.

The Marble persona pack controls identity/delivery text and forbids a sealing
path. [config/voice_marble.md](../config/voice_marble.md) is policy/persona
input, not obsolete Markdown to rewrite during cleanup.
See [utterances](UTTERANCE.md) and [peer interface](THIRD_WAY_MARBLE_PEER.md).
