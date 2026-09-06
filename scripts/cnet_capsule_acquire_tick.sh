#!/usr/bin/env bash
# Policy-approved integer tools only. Intent is demand, never training truth.
set -euo pipefail
umask 077
repo=$(cd "$(dirname "$0")/.." && pwd)
demand=${CNET_CAPSULE_DEMAND_DIR:?required}
policy=${CNET_CAPSULE_TOOL_POLICY:?required}
queue=${CNET_CAPSULE_QUEUE:?required}
capsules=${CNET_CAPSULES_DIR:?required}
refuse() { echo "CAPSULE_ACQUIRE_REFUSED $*" >&2; exit 2; }
private() { [[ ! -L $1 && -O $1 && $(stat -c %a -- "$1") =~ ^[67]00$ ]]; }
for root in "$demand" "$queue" "$capsules"; do
    [[ -d $root ]] && private "$root" || refuse unsafe_directory
done
[[ -f $policy ]] && private "$policy" && [[ $(stat -c %s -- "$policy") -le 65536 ]] || refuse unsafe_policy
[[ ! -L $demand/.acquire.lock ]] || refuse unsafe_lock
if [[ -e $demand/.acquire.lock ]]; then
    [[ -f $demand/.acquire.lock ]] && private "$demand/.acquire.lock" || refuse unsafe_lock
fi
exec 9>"$demand/.acquire.lock"
flock -n 9 || { echo CAPSULE_ACQUIRE_BUSY; exit 3; }
stage=$(mktemp -d "$queue/.acquire-XXXXXX")
cleanup() { rm -f -- "$stage/policy" "$stage/plan" "$stage/request.tsv" "$stage/training.tsv" "$stage/evaluation.tsv" "$stage/tool.txt"; rmdir -- "$stage"; }
trap cleanup EXIT
cp -- "$policy" "$stage/policy"
[[ $(stat -c %s -- "$stage/policy") -le 65536 ]] || refuse oversized_policy
numeric() { [[ $1 =~ ^[0-9]{1,5}$ ]] && ((10#$1 <= 65535)); }
"$repo/bin/cnet_capsule_tool" validate "$stage/policy" || refuse invalid_policy
policy_sha=$(sha256sum "$stage/policy" | cut -d' ' -f1)
tool_sha=$(sha256sum "$repo/bin/cnet_capsule_tool" | cut -d' ' -f1)
shopt -s nullglob
requests=("$demand"/*.req)
((${#requests[@]} <= 256)) || refuse pending_limit
jobs=("$queue"/*)
job_count=${#jobs[@]}
((job_count <= 4096)) || refuse queue_entry_limit
start=0
if [[ -f $demand/.acquire.cursor && ! -L $demand/.acquire.cursor ]] && IFS= read -r last < "$demand/.acquire.cursor"; then
    for i in "${!requests[@]}"; do
        if [[ ${requests[i]##*/} == "$last" ]]; then start=$((i+1)); break; fi
    done
fi
failed=0; processed=0; published=0; examined=0; tool_calls=0
for ((offset=0; offset<${#requests[@]} && examined<64 && published<2 && tool_calls<2048; offset++)); do
    request=${requests[(start+offset)%${#requests[@]}]}
    examined=$((examined+1))
    cursor=$(mktemp "$demand/.cursor-XXXXXX")
    printf '%s\n' "${request##*/}" > "$cursor"
    mv -T -- "$cursor" "$demand/.acquire.cursor"
    if [[ ! -f $request ]] || ! private "$request" || [[ $(stat -c %s -- "$request") -gt 96 ]]; then
        echo CAPSULE_ACQUIRE_REFUSED invalid_demand; failed=1; continue
    fi
    read -r input output value extra < "$request" || { failed=1; continue; }
    if [[ -n ${extra:-} || ! $input =~ ^[a-zA-Z0-9_]{1,31}$ || ! $output =~ ^[a-zA-Z0-9_]{1,31}$ ]] || ! numeric "$value"; then
        echo CAPSULE_ACQUIRE_REFUSED invalid_demand; failed=1; continue
    fi
    value=$((10#$value))
    # Require the complete canonical body, not just an accepted first line.
    if ! cmp -s "$request" <(printf '%s %s %d\n' "$input" "$output" "$value"); then
        echo CAPSULE_ACQUIRE_REFUSED noncanonical_demand; failed=1; continue
    fi
    # Never consume a partial stream; exit 3 means no path within eight hops.
    if "$repo/bin/cnet_capsule_tool" plan "$stage/policy" "$input" "$output" "$value" > "$stage/plan"; then
        plan_status=0
    else plan_status=$?; fi
    if ((plan_status)); then
        if ((plan_status == 3)); then
            echo "CAPSULE_ACQUIRE_REFUSED no_approved_path input=$input output=$output value=$value terminal=1"
            rm -- "$request"
        else echo CAPSULE_ACQUIRE_REFUSED planner_failure_retryable; fi
        failed=1; continue
    fi
    complete=1
    # Existing durable edges consume work, but not the two-new-job allowance.
    # A path longer than two edges therefore advances across successive ticks.
    while read -r input output ib ob op operand lo hi value expected; do
    low=$((value/32*32)); high=$((low+31))
    ((low >= lo)) || low=$lo
    ((high <= hi)) || high=$hi
    identity=$(printf '%s %s %s %s %d %d\n' "$policy_sha" "$tool_sha" "$input" "$output" "$low" "$high" | sha256sum | cut -d' ' -f1)
    unit="tool_${identity:0:48}"
    job="$queue/$unit"
    if [[ ! -e $job && ! -L $job ]] && ((published >= 2)); then
        echo CAPSULE_ACQUIRE_DEFERRED publication_budget; complete=0; break
    fi
    if [[ ! -e $job && ! -L $job ]] && ((job_count >= 4096)); then
        echo CAPSULE_ACQUIRE_REFUSED queue_entry_limit; failed=1; complete=0; break
    fi
    if ((tool_calls + high-low+1 > 2048)); then
        echo CAPSULE_ACQUIRE_DEFERRED evidence_work_budget; complete=0; break
    fi
    printf '%s %s %s %d %d verified_tool\n' "$unit" "$input" "$output" "$ib" "$ob" > "$stage/request.tsv"
    : > "$stage/training.tsv"; : > "$stage/evaluation.tsv"
    valid=1
    for ((x=low; x<=high; x++)); do
        tool_calls=$((tool_calls+1))
        if ! y=$("$repo/bin/cnet_capsule_tool" "$op" "$operand" "$x") || ! numeric "$y" || ((10#$y >= (1<<ob))); then valid=0; break; fi
        printf '%d %d\n' "$x" "$y" >> "$stage/training.tsv"
        printf '%s %s %d %d\n' "$input" "$output" "$x" "$y" >> "$stage/evaluation.tsv"
    done
    if ((!valid)); then echo CAPSULE_ACQUIRE_REFUSED tool_overflow_or_failure; failed=1; complete=0; break; fi
    printf 'policy_sha256=%s\ntool_sha256=%s\nrows=%d\n' "$policy_sha" "$tool_sha" "$((high-low+1))" > "$stage/tool.txt"
    if [[ -e $job || -L $job ]]; then
        valid=1
        [[ -d $job ]] && private "$job" || valid=0
        for file in request.tsv training.tsv evaluation.tsv tool.txt; do
            [[ -f $job/$file ]] && private "$job/$file" && cmp -s "$stage/$file" "$job/$file" || valid=0
        done
        if ((!valid)); then echo CAPSULE_ACQUIRE_REFUSED conflicting_job; failed=1; complete=0; break; fi
    else
        jobstage=$(mktemp -d "$queue/.job-XXXXXX")
        for file in request.tsv training.tsv evaluation.tsv tool.txt; do cp -- "$stage/$file" "$jobstage/$file"; done
        # Same owner is the local trust boundary. No concurrent acquisition
        # writer can replace this immutable deterministic job under our lock.
        sync -f "$jobstage"
        mv -T -n -- "$jobstage" "$job"
        [[ ! -d $jobstage ]] || refuse publication_conflict
        sync -f "$queue"
        job_count=$((job_count+1))
        published=$((published+1))
    fi
    done < "$stage/plan"
    # Only a COMPLETE set of durable jobs owns retries from this point onward.
    if ((complete)); then rm -- "$request"; processed=$((processed+1)); fi
done
# Also advance manual/previous jobs even when a new demand was unsupported.
if ! bash "$repo/scripts/cnet_capsule_curriculum_tick.sh"; then failed=1; fi
printf 'CAPSULE_ACQUIRE_TICK processed=%d published=%d examined=%d tool_calls=%d failed=%d\n' "$processed" "$published" "$examined" "$tool_calls" "$failed"
exit "$failed"
