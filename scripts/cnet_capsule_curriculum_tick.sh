#!/usr/bin/env bash
# Trusted local evidence jobs. Called by the existing autoteach scheduler.
# No own-answer log ingestion, shell evaluation, network, or certification bypass.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
capsules=${CNET_CAPSULES_DIR:?CNET_CAPSULES_DIR required}
queue=${CNET_CAPSULE_QUEUE:?CNET_CAPSULE_QUEUE required}
limit=${CNET_CAPSULE_JOBS_PER_TICK:-2}
scan_limit=${CNET_CAPSULE_SCAN_PER_TICK:-64}
[[ $limit =~ ^[1-8]$ && -d $queue && ! -L $queue && -d $capsules && ! -L $capsules ]] || exit 2
[[ $scan_limit =~ ^[1-9][0-9]{0,2}$ ]] && ((scan_limit <= 256)) || exit 2
exec 8>"$queue/.tick.lock"
flock -n 8 || { echo CAPSULE_CURRICULUM_BUSY; exit 3; }
processed=0
examined=0
failed=0
snapshot=''
cleanup_snapshot() {
    if [[ -n $snapshot ]]; then
        rm -f -- "$snapshot/request.tsv" "$snapshot/training.tsv" "$snapshot/evaluation.tsv"
        rmdir -- "$snapshot"
        snapshot=''
    fi
}
trap cleanup_snapshot EXIT
shopt -s nullglob
jobs=("$queue"/*)
# Bash still enumerates the directory once; keep this trusted local queue small.
# The cap refuses oversized queues before file hashing, copying, or teaching.
(( ${#jobs[@]} <= 4096 )) || { echo CAPSULE_CURRICULUM_REFUSED queue_entry_limit; exit 2; }
start=0
# Rotate after the last examined entry, including completed/invalid jobs. Evidence
# remains retryable when its dependencies change without starving later jobs.
if [[ -f $queue/.cursor ]] && IFS= read -r -d '' last_job < "$queue/.cursor"; then
    for i in "${!jobs[@]}"; do
        if [[ ${jobs[i]##*/} == "$last_job" ]]; then start=$((i+1)); break; fi
    done
fi
for ((offset=0; offset<${#jobs[@]}; offset++)); do
    ((examined < scan_limit && processed < limit)) || break
    cleanup_snapshot
    job=${jobs[(start+offset)%${#jobs[@]}]}
    examined=$((examined+1))
    cursor=$(mktemp "$queue/.cursor-XXXXXX")
    printf '%s\0' "${job##*/}" > "$cursor"
    mv -T "$cursor" "$queue/.cursor"
    [[ -d $job && ! -L $job ]] || { failed=1; continue; }
    valid=1
    for file in request.tsv training.tsv evaluation.tsv; do
        [[ -f $job/$file && ! -L $job/$file && $(stat -c %s "$job/$file") -le 65536 ]] || valid=0
    done
    if (( !valid )); then echo CAPSULE_CURRICULUM_REFUSED invalid_job_files; failed=1; continue; fi
    # Hash, parse, train and evaluate one private copy, never mutable queue files.
    snapshot=$(mktemp -d "$queue/.evidence-XXXXXX")
    for file in request.tsv training.tsv evaluation.tsv; do
        cp -- "$job/$file" "$snapshot/$file"
        [[ $(stat -c %s "$snapshot/$file") -le 65536 ]] || valid=0
    done
    if (( !valid )); then echo CAPSULE_CURRICULUM_REFUSED oversized_snapshot; failed=1; continue; fi
    read -r unit input output ib ob source extra < "$snapshot/request.tsv" || { failed=1; continue; }
    if [[ -n ${extra:-} || ! $unit =~ ^[a-zA-Z0-9_]+$ || ! $input =~ ^[a-zA-Z0-9_]+$ ||
          ! $output =~ ^[a-zA-Z0-9_]+$ || ! $ib =~ ^[0-9]+$ || ! $ob =~ ^[0-9]+$ ||
          ! $source =~ ^(verified_tool|user_correction)$ || $(wc -l < "$snapshot/request.tsv") -ne 1 ]]; then
        echo CAPSULE_CURRICULUM_REFUSED invalid_request_or_provenance; failed=1; continue
    fi
    # Relative names make receipts stable across staging paths and queue moves.
    digest=$(cd "$snapshot" && sha256sum request.tsv training.tsv evaluation.tsv | sha256sum | cut -d' ' -f1)
    if [[ -f $job/receipt.txt && -d $capsules/$unit ]] && grep -qx "evidence_sha256=$digest" "$job/receipt.txt"; then continue; fi
    receipt=$(mktemp "$job/.receipt-XXXXXX")
    if CNET_CAPSULE_EVAL_FILE="$snapshot/evaluation.tsv" timeout 120 "$repo/bin/cnet_capsule_core" teach \
        "$capsules" "$unit" "$input" "$output" "$ib" "$ob" "$source" "$snapshot/training.tsv" > "$receipt" 2>&1; then
        printf 'evidence_sha256=%s\n' "$digest" >> "$receipt"
        mv -T "$receipt" "$job/receipt.txt"
        cat "$job/receipt.txt"
    else
        mv -T "$receipt" "$job/refusal.txt"
        cat "$job/refusal.txt"
        failed=1
    fi
    processed=$((processed+1))
done
printf 'CAPSULE_CURRICULUM_TICK processed=%s failed=%s budget=%s examined=%s scan_budget=%s\n' "$processed" "$failed" "$limit" "$examined" "$scan_limit"
exit "$failed"
