# cnetd — warm UNIX socket front door

## Socket

1. `$XDG_RUNTIME_DIR/cnet/cnet.sock` (preferred)
2. `~/.local/share/cnet-minimal/run/cnet.sock` (fallback)
3. `CNET_SOCK` override

## Commands

```bash
make cnetd
make cnetd-run                    # build + start + smoke
scripts/cnet_sock_ask.sh "who are you"
echo 'PING' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/cnet/cnet.sock
```

## Protocol

```
ASK <query>
→ SOURCE … / ANSWER … / END

{"op":"ask","q":"..."}
→ JSON one line

PING → PONG
STATUS → OK cnetd …
```

## Env

`CNET_MINIMAL_ROOT`, `CNET_PACKS_ROOT`, probe short-circuit, domain routes.

## systemd

```bash
cp scripts/systemd/cnetd.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now cnetd.service
```

Law: never self-CERT; probes short-circuit with `SHORTCIRCUIT 1`.
