# Domain routing

The [domain-route API](../include/cnet_domain_route.h) selects a route category.
It does not execute a model, install a cartridge or grant trust.

Selection checks CERT-pack rules, then MTK rules, then base-GGUF rules;
otherwise it returns ABSTAIN. The longest eligible pattern wins within a tier.
Matching uses word boundaries and minimum pattern length; the default minimum
is four characters. `conf_x1000` is telemetry, not a certification score.

Compiled defaults are always available. The optional
[TSV overlay](../config/domain_routes.tsv) uses fixed slots, with a maximum of
256 rules in the public router. Loading can soft-fail while retaining defaults
and truncates excess rules: inspect the load result, `loaded_file` and rule
count rather than assuming a requested overlay was fully installed.
Matching performs no dynamic allocation.

```sh
make domain_route
bin/roe_domain_route 'who are you'
bin/roe_domain_route 'completely unknown domain xyzzy'
```

A route decision naming an MTK is not proof it was applied. MTK deltas and
certified portable capsules remain distinct; see [dispatch](dispatch.md).
