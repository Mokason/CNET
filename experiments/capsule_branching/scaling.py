"""Bounded, explicit dependency plans over independently loaded native capsules.

Only the declared state6 -> next6 interface and sum-mod-64 edge adapter are
supported. The adapter and final sum are experimental checked Python tools.
"""
from dataclasses import dataclass
from branching import Context, Refusal, Spec


@dataclass(frozen=True)
class Node:
    index: int
    spec: Spec
    literal: int | None
    parents: tuple


@dataclass(frozen=True)
class Receipt:
    node: Node
    request_id: str
    context: Context
    input_value: int
    value: int
    hops: int
    units: str


@dataclass(frozen=True)
class Result:
    value: int
    output_ids: tuple
    request_id: str
    context: Context
    receipts: tuple
    issued: tuple


def validate_plan(nodes, outputs):
    if not 1 <= len(nodes) <= 32 or not outputs or len(set(outputs)) != len(outputs):
        raise Refusal('invalid plan size or outputs')
    seen = set()
    for n in nodes:
        if type(n.index) is not int or n.index in seen:
            raise Refusal('duplicate or invalid node')
        if n.spec.input_tag != 'state6' or n.spec.output_tag != 'next6':
            raise Refusal('unsupported typed adapter')
        if len(n.parents) > 2 or len(set(n.parents)) != len(n.parents):
            raise Refusal('invalid fan-in')
        if n.parents:
            if n.literal is not None or any(p not in seen for p in n.parents):
                raise Refusal('missing, forward or cyclic dependency')
        elif type(n.literal) is not int or not 0 <= n.literal < 64:
            raise Refusal('uncovered literal')
        seen.add(n.index)
    if any(i not in seen for i in outputs):
        raise Refusal('missing output node')


def verify(result):
    """Local receipt consistency only; this is not proof authentication."""
    validate_plan([r.node for r in result.receipts], result.output_ids)
    if result.receipts != result.issued:
        raise Refusal('unissued or altered receipt')
    seen = {}
    for r in result.receipts:
        if r.request_id != result.request_id or r.context != result.context:
            raise Refusal('incompatible request or context')
        if type(r.value) is not int or not 0 <= r.value < 64 or r.hops < 1 or not r.units:
            raise Refusal('unverified typed result')
        expected = sum(seen[p] for p in r.node.parents) % 64 if r.node.parents else r.node.literal
        if r.input_value != expected:
            raise Refusal('dependency value mismatch')
        seen[r.node.index] = r.value
    if result.value != sum(seen[i] for i in result.output_ids):
        raise Refusal('final join mismatch')


def execute(cache, nodes, outputs, request_id, context):
    validate_plan(nodes, outputs)
    if not request_id or not all((context.entity, context.regime, context.hypothesis)):
        raise Refusal('missing request context')
    seen = {}
    for n in nodes:
        value = sum(seen[p].value for p in n.parents) % 64 if n.parents else n.literal
        # Dependents retain copied receipts, not pointers into evictable cores.
        with cache.pin(n.spec):
            answer, hops, units = cache.ask(n.spec, value)
        if type(answer) is not int or not 0 <= answer < 64 or hops < 1 or not units:
            raise Refusal('unverified branch')
        seen[n.index] = Receipt(n, request_id, context, value, answer, hops, units)
    receipts = tuple(seen.values())
    result = Result(sum(seen[i].value for i in outputs), tuple(outputs), request_id,
                    context, receipts, receipts)
    verify(result)
    return result
