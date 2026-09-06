# Marble peer interface

The daemon's PEER request form carries a request-local peer name plus query.
It shares the [socket protocol](CNETD.md), capsule checks, pack lookup and
presentation boundary with ordinary requests.

`bin/cnet_peer` is the native client. Select `CNET_SOCK` deliberately before
using it against an installed daemon. A peer name is metadata, not an
authenticated identity or a separate protected session.

Hermes can be a peer client; it is not required for local capsule operation.
The MCP server exposes tool/factory interfaces separately. Do not treat MCP
tool availability as proof of a live peer conversation or vice versa.

Persona packs and templates affect delivery. They do not establish feelings,
change coverage or permit self-certification. The daemon's legacy residual
chat branch remains hard-disabled; proposed optional residual behavior in
older plans is not current runtime behavior.

Queries can include mutating chat actions. Protect peer/socket access with
the same trusted-operator boundary as the [web client](CNET_WEB.md).
