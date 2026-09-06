# Enable live capsule acquisition

User authorized live activation. Keep the existing autoteach timer cadence,
two-new-job bound, owner-private spool/policy and certification/history gates.
Activate the previously tested integer tools plus the existing time/frame
interfaces, with unchanged port widths. No new network teacher integration.

- [x] Confirm live missing capture (RED) and an existing composed answer.
- [x] Rebuild and pass isolated socket acquisition preflight.
- [x] Preserve pre-activation environment and immutable capsules.
- [x] Install private demand directory and approved policy; enable environment.
- [x] Restart only cnetd; restore existing autoteach timer and run its real service.
- [x] Verify live miss → queued demand → scheduled worker → certified answers,
  old answers, refusals, restart persistence and deployed learner health.

Evidence: [live acquisition results](../result/cnet_live_acquisition_20260906.md).
The scheduled worker was exercised through two explicit starts of the actual
systemd service; the timer remains active at its existing cadence. This is not
a claim that a subsequent unattended timer firing was observed.

Rollback: restore `config/certified-core-deployment.env` from
`artifacts/deployments/live-acquisition-20260906/before.env`, then restart cnetd.
This disables further automatic acquisition without deleting learned capsules
or changing the active learner base. Do not overwrite capsules/base while
writers run. Snapshot `capsules-before` preserves the original two capsules.
