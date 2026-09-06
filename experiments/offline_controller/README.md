# Isolated offline controller experiment

Run `bash run.sh` for the frozen three-seed CPU-verified, dual-AMD comparison.
Add `--local-baseline` to query the already-running local model at
`127.0.0.1:8092`. Output goes to a fresh `/tmp/cnet-controller-run-*` directory.
Nothing is installed or promoted into CNET's serving path.

`make test gpu-test` runs the focused gates. `node audit.mjs ARTIFACT_DIRECTORY`
independently verifies recorded paths; `bash test_audit.sh ARTIFACT_DIRECTORY`
proves that a forged acceptance is rejected. Assertions are required at build
time; GPU or network-boundary failures must not silently fall back.

Decision and measured limitations:
[`../../result/cnet_offline_controller_20260906.md`](../../result/cnet_offline_controller_20260906.md).
The experiment does **not** justify promoting the custom controller.
