# Brain interoperability

Two independent formats meet at an explicit boundary:

| Artifact | Meaning |
| --- | --- |
| Brain `pieces.bin` / `*.cap` | Continuous linear-map geometry |
| CNET `unit.cnb` + `manifest.cknow` | Sealed unit, typed contract and coverage |

[brain_to_cnet_capsule.c](../tools/brain_to_cnet_capsule.c) derives discrete
mode identities and center-bit signatures from Brain geometry. This does not
certify arbitrary continuous Brain outputs or introduce a second CNET package.

## Local gates

```sh
make brain_cnet_capsule brain_sidecar cnet_capsule_step
```

The conversion gate produces `brain_mode_id` and `brain_center_sig` under
the configured artifact output. Inspect the tool's argument handling before
importing a real bundle; use a new private destination.

The [sidecar API](../include/cnet_brain_sidecar.h) loads continuous CBPC data
separately. `CNET_BRAIN_SIDECAR` is opt-in; missing, corrupt or incompatible
data must not become a certified answer. The sidecar does not admit its
continuous outputs into CNU1.

`bin/cnet_capsule_step` is the small execution host for exported discrete
capsules. External Brain executables, bundle paths and hybrid deployment belong
to that host's installation, not this repository's build. For the canonical
CNET packaging and teaching workflow, use [capsule core](CAPSULE_CORE.md).
