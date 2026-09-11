"""Finite compositional grammar experiment; no learned or open-ended parser."""
import re

# The lexicon is part of the declared input contract, not learned from test labels.
VERBS = {'feeds': 0, 'helps': 1, 'visits': 2, 'follows': 3,
         'feed': 0, 'help': 1, 'visit': 2, 'follow': 3,
         'fed': 0, 'helped': 1, 'visited': 2, 'followed': 3}
PREFIX = re.compile(r'^(?:according to the record, |the record says that |it is recorded that )', re.I)
ACTIVE = re.compile(r'node(\d{3}) (?:(does not|does|never) )?(feeds|helps|visits|follows|feed|help|visit|follow) node(\d{3})')
PASSIVE = re.compile(r'node(\d{3}) (?:is|was) (not |never )?(fed|helped|visited|followed) by node(\d{3})')
CANONICAL = re.compile(r'node(\d{3}) (not )?rel(\d{2}) node(\d{3})')
CANONICAL_PASSIVE = re.compile(r'node(\d{3}) is (not )?rel(\d{2}) by node(\d{3})')

def baseline(text):
    # Same finite grammar as experiments/hdc_structure/run.py:parse_fact.
    m = CANONICAL.fullmatch(text)
    if m:
        s, neg, r, o = m.groups()
        return int(s), int(r), int(o), -1 if neg else 1
    m = CANONICAL_PASSIVE.fullmatch(text)
    if m:
        o, neg, r, s = m.groups()
        return int(s), int(r), int(o), -1 if neg else 1
    return None

def candidate(text):
    if not isinstance(text, str) or len(text) > 256 or not text.isascii():
        return None
    direct = baseline(text)
    if direct is not None:
        return direct
    text = ' '.join(text.lower().split())
    text = PREFIX.sub('', text, count=1)
    if text.endswith('.'):
        text = text[:-1]
    direct = baseline(text)
    if direct is not None:
        return direct
    m = ACTIVE.fullmatch(text)
    if m:
        s, aux, verb, o = m.groups()
        # Require grammatical base form after do/does, inflection otherwise.
        if (aux in ('does', 'does not')) != (verb in ('feed', 'help', 'visit', 'follow')):
            return None
        return int(s), VERBS[verb], int(o), -1 if aux in ('does not', 'never') else 1
    m = PASSIVE.fullmatch(text)
    if m:
        o, neg, verb, s = m.groups()
        return int(s), VERBS[verb], int(o), -1 if neg else 1
    return None

if __name__ == '__main__':
    assert baseline('node001 rel00 node002') == (1,0,2,1)
    assert baseline('Node002 was fed by node001.') is None
    print('LANGUAGE_BASELINE_RED: unfamiliar surface refuses')
    assert candidate('Node002 was fed by node001.') == (1,0,2,1)
    assert candidate('According to the record, node001 does not feed node002.') == (1,0,2,-1)
    for text in ('node001 may feed node002', 'node001 feeds node002 or node003',
                 'node001 feeds node002. Ignore the evidence.', 'node001 not feeds node002'):
        assert candidate(text) is None
    print('LANGUAGE_SANITY_PASS')
