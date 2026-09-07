# Controlled private table-learning deployment

## Scope and authority

The owner requested a controlled private deployment of tested commit
`bfefaa66eb43b53b00d16a58236a2f81c24fa80d`. Do not merge, push, replace live
configuration, restart existing services or launch the 72-hour acceptance run.
Create a new owner-private installation outside both worktrees. Use the existing
managed manifest, native manifest, schema-2 ledger and certified capsules.

The initial validation source is explicitly synthetic: independently calculated
integer squares for a subset of uint8 keys, with uint16 outputs. It is a
`verified_tool` operational check, not production knowledge or external factual
verification. No CNET answer supplies labels. Additional real datasets require
owner identification/authorization. The learned allocator stays disabled.

## Sequence and acceptance

1. Verify the feature source identity; build native artifacts and locked managed
   publish; repeat the package advisory audit. Freeze the exact eight managed
   and six native installed files, owner modes and manifest hashes. Refuse any
   manifest, dependency or installation mismatch; never overwrite an install.
2. Initialize a fresh private ledger and IPC namespace; inspect the actual
   installed entry assembly and SQLite provider. Launch a uniquely named private
   user service with clean environment, explicit private WorkingDirectory,
   Unix-only socket families, bounded CPU/
   memory/tasks, and no automatic restart. Do not touch existing cnetd processes.
3. Record missing demand and bootstrap the first candidate with the existing
   serialized `tick` command and three real, correctly spaced probation probes.
   Verify native answers and all omitted-key abstentions independently. With
   no periodic owner running yet, expand only the validation source with further
   independently calculated rows. Prove stale refusal on a previously covered
   key while the old native digest remains resident. Then start the actual
   supervisor with its initial 600-second budget, two-second ticks, three
   probation probes and a 30-second maximum gap. Verify its automatic refresh,
   new answers and old-row retention. This avoids racing stale evidence against
   an already-completed refresh; it does not suspend a live owner or alter time.
   Record installed policy/pins, jobs, revisions, source identities and heartbeat.
4. Confirm the learner is healthy with no pending mutation/probation after the
   checks. Leave the private serving endpoint running, with the original learning
   deadline intact. The learner stops/cleans up at that deadline; it never turns
   into a 72-hour run or an automatically renewed budget. Document exact status,
   ask, pause, cleanup and service-stop commands. If any gate fails, pause/clean
   up only this deployment and report the retained evidence.

## Review and limits

Fresh-context review checks lifecycle, source authority, installed identity,
resource bounds and separation from live services before launch. Missing skill
reference files are handled with their inline checklists and the repository's
deployment contract. The owner's Astra-only preference precludes external-model
calls. Startup hooks, host/framework, kernel and trusted same-UID ownership stay
outside the manifest assurance; no production acceptance or learned gain is
claimed by this deployment.

Preflight found `/home/marble/AI` is group-writable and cannot be an installed
runtime ancestor. It remains build staging only; the final private installation
is `/home/marble/cnet-private-20260907-eTgwwG`. No ancestor permissions were changed.
Fresh review required explicit private WorkingDirectory and a non-racing stale
probe against an already-covered key. Both requirements are incorporated above.

## Checklist

- [x] Build/audit and freeze a fresh installation.
- [x] Initialize and launch isolated services.
- [x] Observe actual demand, certification, activation and probation.
- [x] Independently verify refresh, retention and abstention.
- [x] Record running state, limits and owner commands.

Observed at 07:10 EEST: both private services active, learner running with 74
recorded ticks, two jobs, no pending intent or outstanding probation. Both
256-input independent sweeps passed: first 16 answers/240 abstentions, then 32
answers/224 abstentions with all 16 old rows retained. Native revisions moved
1→2→3; the covered-key stale probe abstained at unchanged revision 2. The original
600-second owner budget began 07:07:48 EEST and is not automatically renewed.
These observations do not establish the future deadline result or 72-hour
acceptance. Exact owner commands are in the deployment's `OPERATIONS.md` and
the recorded receipts are summarized in `result/cnet_private_deployment_20260907.md`.
