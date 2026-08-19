# Evolve direction (curriculum)

Steer unattended CORE growth without mid-loop babysitting.

## Config search order

1. `$CNET_EVOLVE_DIRECTION`
2. `$CNET_CORE_BUS_BRICKS_DIR/evolve_direction.conf`
3. `config/cnet_evolve_direction.conf`
4. built-in defaults

## Keys

| Key | Meaning |
|-----|---------|
| `allow_live_miss` | Admit complete taught domains from miss_log |
| `allow_factory` | Build factory curriculum from GGUF tensors |
| `allow_goals` | Process pending_goals + direction `goal=` lines |
| `factory_if_empty` | Seed factory when bank empty |
| `max_new_per_tick` | Cap new bricks per evolve run |
| `prefer_domains` | Only these live domains (empty=all) |
| `deny_domains` | Never admit these |
| `factory=tag,mode,tensor` | Curriculum brick |
| `goal=...` | CERT-only goal seed each tick |

## Example

```bash
cp config/cnet_evolve_direction.conf $CNET_CORE_BUS_BRICKS_DIR/evolve_direction.conf
# edit prefer_domains=live_dom,user_pref
# edit factory=... lines
./bin/cnet_core_evolve --once
```
