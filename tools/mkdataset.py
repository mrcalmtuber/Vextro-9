#!/usr/bin/env python3
"""Build the training set for the grounded-answer model, from the archive.

The model being trained has one job: read a question and a passage from
Wikipedia, and answer in plain English *using only that passage*. So
every target here is built out of the passage it is paired with. Nothing
is invented, by construction -- a dataset written by a larger model would
teach this one to sound like that one, including when it was wrong.

Three shapes, and the third matters as much as the others:

  definition   "What is X?" answered from the article's own opening
  fact         a question about something the passage states
  refusal      a question the passage does not answer, whose target is
               "Not in the archive."

Without refusals a small model learns that an answer is always available
and will produce one from nowhere, which is the failure this whole
pipeline exists to prevent. A fifth of the set is refusals.

    python3 tools/mkdataset.py assets/wiki.zim build/dataset.jsonl 24000
"""
import json
import random
import re
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from zimread import Zim, html_text          # noqa: E402

STOP_TITLE = re.compile(r'^(List of|Wikipedia:|Category:|Template:|File:|'
                        r'Help:|Portal:|Module:|Draft:)')
COPULA = re.compile(r'^(.{0,90}?)\b(is|are|was|were)\b\s+(.+)$', re.S)

# Navigation furniture that survives the HTML stripper and reads like a
# sentence without being one. Left in, it teaches the model to answer
# "What is Avatar?" with "For the 2009 film, see Avatar (2009 movie)."
HATNOTE = re.compile(
    r'^(This article is about|This page is about|For other uses|For the |'
    r'Not to be confused|See also|.{0,40} may refer to|.{0,40} redirects here)',
    re.I)
CITATION = re.compile(r'(Retrieved|ISBN|doi:|\(in \w+\)|Archived|\d{4}-\d\d-\d\d)')


def clean_lead(text, title):
    """The article's prose opening, with the markup's stutter removed."""
    # The markup repeats the title two or three times before the prose,
    # but the *last* one usually opens the first sentence -- "British
    # American Tobacco (BAT) is a large..." -- so stripping every copy
    # decapitates the definition into "(BAT) is a large...". Leave one.
    t = text
    # A disambiguated title -- "Trent's Last Case (1929 movie)" -- is not
    # what the prose repeats; the bare name is. Try both.
    names = [title]
    bare = re.sub(r'\s*\([^)]*\)\s*$', '', title)
    if bare and bare != title:
        names.append(bare)

    def eat(x):
        for nm in names:
            if x[:len(nm)].lower() == nm.lower():
                return x[len(nm):].lstrip(), nm
        return None, None

    while True:
        nxt, nm = eat(t)
        if nxt is None:
            break
        again, _ = eat(nxt)
        if again is None:
            break          # this copy opens the first sentence; keep it
        t = nxt
    return t


def sentences(text, limit=12):
    out = []
    for part in re.split(r'(?<=[.!?])\s+', text):
        part = part.strip()
        if not part:
            continue
        out.append(part)
        if len(out) >= limit:
            break
    return out


def is_prose(s):
    """Sentences, not infobox rows, hatnotes or citation debris."""
    words = re.findall(r'[A-Za-z0-9]+', s)
    if len(words) < 6 or len(words) > 60:
        return False
    capped = sum(1 for w in words if w[0].isupper())
    if capped * 10 >= len(words) * 4:
        return False
    # infobox text runs words together: "DesignationsAlternative namesLuna"
    if re.search(r'[a-z]{2}[A-Z][a-z]{2}[A-Z]', s):
        return False
    if len(s) > 400 or HATNOTE.match(s) or CITATION.search(s):
        return False
    # a reference list or a table of numbers is not a statement
    digits = sum(1 for c in s if c.isdigit())
    if digits * 5 > len(s):
        return False
    if s.count('"') > 2 or not s.endswith(('.', '!', '?')):
        return False
    # infobox rows that slipped past the camel-case test
    if re.search(r'(•|/km2|UTC[+-]|Postal code|Elevation\d)', s):
        return False
    return True


def definition_target(title, sent):
    """The lead sentence, when it actually defines the subject.

    Taken verbatim rather than rewritten to start with the title. An
    earlier version rebuilt it as "<title> is <predicate>", which
    required the whole title to appear in the subject and so threw away
    most of the archive: the entry called "Big Star (Kenny Chesney
    song)" opens "Big Star is a song written by...", which is already
    the right answer and does not need reassembling. Extracting is also
    the behaviour being taught -- answer in the source's words.
    """
    if not COPULA.match(sent):
        return None
    # it has to be about the subject: share a distinctive word with it
    tw = [w for w in re.findall(r'[A-Za-z]{4,}', title)]
    if tw and not any(w.lower() in sent.lower() for w in tw):
        return None
    return sent


def content_words(s):
    small = {'the', 'and', 'that', 'this', 'with', 'from', 'have', 'has',
             'was', 'were', 'are', 'for', 'its', 'they', 'them', 'their',
             'which', 'when', 'where', 'been', 'also', 'some', 'other',
             'into', 'more', 'most', 'than', 'then', 'these', 'those'}
    return [w for w in re.findall(r'[A-Za-z]{4,}', s)
            if w.lower() not in small]


def main():
    path = sys.argv[1]
    out_path = sys.argv[2]
    want = int(sys.argv[3]) if len(sys.argv) > 3 else 24000

    z = Zim(path)
    rng = random.Random(20260808)

    # A spread across the archive rather than the first N alphabetically,
    # so the set is not all articles beginning with A.
    order = list(range(z.article_count))
    rng.shuffle(order)

    examples, leads = [], []
    scanned = 0

    for idx in order:
        if len(examples) >= want:
            break
        scanned += 1
        if scanned > want * 12:
            break
        try:
            e = z.dirent(idx)
        except Exception:
            continue
        if e['ns'] != 'C' or e['redirect'] is not None:
            continue
        title = e['title']
        if not title or STOP_TITLE.match(title) or len(title) > 48:
            continue
        try:
            _, blob = z.content(idx)
        except Exception:
            continue
        if not blob or len(blob) < 400:
            continue

        text = clean_lead(html_text(blob, 24000), title)
        sents = [s for s in sentences(text) if is_prose(s)]
        if len(sents) < 2:
            continue

        evidence = ' '.join(sents[:3])[:600]
        if len(evidence) < 120:
            continue
        leads.append((title, evidence))

        # --- 1. definition, the question people actually ask ---------
        tgt = definition_target(title, sents[0])
        if tgt and len(tgt) < 300:
            # "Who was" when the lead describes a person
            person = re.search(r'\bwas an?\b', sents[0]) and \
                re.search(r'\b(1[0-9]{3}|20[0-2][0-9])\b', evidence)
            q = ('Who was %s?' if person else
                 ('What are %s?' if title.endswith('s') else 'What is %s?'))
            examples.append({
                'question': q % title,
                'context': '[%s] %s' % (title, evidence),
                'answer': tgt,
                'kind': 'definition',
            })

        # --- 2. an open request, answered with the passage's own lead -
        if len(examples) < want and rng.random() < 0.5:
            body = ' '.join(sents[:2])
            if len(body) < 340:
                examples.append({
                    'question': 'Tell me about %s' % title,
                    'context': '[%s] %s' % (title, evidence),
                    'answer': body,
                    'kind': 'about',
                })

        # --- 3. a specific fact the passage states -------------------
        if len(sents) >= 2 and len(examples) < want and rng.random() < 0.35:
            s2 = sents[1]
            words = content_words(s2)
            if words and len(s2) < 260:
                subject = rng.choice(words)
                examples.append({
                    'question': 'What does %s have to do with %s?'
                                % (title, subject.lower()),
                    'context': '[%s] %s' % (title, evidence),
                    'answer': s2,
                    'kind': 'fact',
                })

    # --- 3. refusals: a question whose evidence is about something else
    n_refuse = max(1, len(examples) // 4)
    for _ in range(n_refuse):
        (t1, _), (t2, ev2) = rng.sample(leads, 2)
        if t1.lower() in ev2.lower():
            continue
        examples.append({
            'question': 'What is %s?' % t1,
            'context': '[%s] %s' % (t2, ev2),
            'answer': 'Not in the archive.',
            'kind': 'refusal',
        })

    rng.shuffle(examples)
    with open(out_path, 'w') as f:
        for ex in examples:
            f.write(json.dumps(ex) + '\n')

    kinds = {}
    for ex in examples:
        kinds[ex['kind']] = kinds.get(ex['kind'], 0) + 1
    print('%d examples from %d entries scanned -> %s'
          % (len(examples), scanned, out_path))
    for k in sorted(kinds):
        print('  %-11s %d' % (k, kinds[k]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
