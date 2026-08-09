#!/usr/bin/env python3
"""Measure the fine-tuned model against the one it started from.

Three numbers, and the first is the one that matters here:

  grounded   the fraction of answers every content word of which appears
             in the passage the model was given. This is the same rule
             the kernel's verifier applies, so it predicts how often an
             answer will survive to the screen.
  refusal    on examples whose passage does not answer the question, how
             often the model says so instead of inventing something.
  overlap    token F1 against the reference answer -- a rough measure of
             whether it picked the right part of the passage.

    build/venv/bin/python tools/eval_explainer.py \\
        build/explainer.holdout.json build/smol-base build/explainer
"""
import json
import re
import sys

import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from train_explainer import build_prompt      # noqa: E402

STOP = {'the', 'and', 'that', 'this', 'with', 'from', 'have', 'has', 'was',
        'were', 'are', 'for', 'its', 'they', 'them', 'their', 'which',
        'when', 'where', 'been', 'also', 'some', 'other', 'into', 'more',
        'most', 'than', 'then', 'these', 'those', 'what', 'who', 'about'}


def content(s):
    return [w.lower() for w in re.findall(r'[A-Za-z]{4,}', s)
            if w.lower() not in STOP]


def stem_in(hay, word):
    """The kernel's rule: a stem of at least four characters, on a word
    boundary."""
    n = max(4, len(word) - 3)
    return re.search(r'(?<![A-Za-z0-9])' + re.escape(word[:n]), hay,
                     re.I) is not None


def grounded(answer, context):
    words = content(answer)
    if not words:
        return False
    bad = sum(0 if stem_in(context, w) else 1 for w in words)
    return bad == 0 or (bad <= 3 and bad * 3 <= len(words))


def f1(pred, ref):
    p, r = content(pred), content(ref)
    if not p or not r:
        return 0.0
    common = 0
    rr = list(r)
    for w in p:
        if w in rr:
            rr.remove(w)
            common += 1
    if common == 0:
        return 0.0
    prec, rec = common / len(p), common / len(r)
    return 2 * prec * rec / (prec + rec)


def run(model_dir, rows, dev, limit):
    tok = AutoTokenizer.from_pretrained(model_dir)
    mod = AutoModelForCausalLM.from_pretrained(model_dir, dtype=torch.float32)
    mod.to(dev).eval()

    n_ground = n_ref_ok = n_ref = 0
    tot_f1 = 0.0
    samples = []

    for i, r in enumerate(rows[:limit]):
        prompt = build_prompt(tok, r['question'], r['context'])
        ids = tok(prompt, return_tensors='pt', add_special_tokens=False)
        ids = {k: v.to(dev) for k, v in ids.items()}
        with torch.no_grad():
            out = mod.generate(**ids, max_new_tokens=64, do_sample=False,
                               pad_token_id=tok.pad_token_id or 0)
        text = tok.decode(out[0][ids['input_ids'].shape[1]:],
                          skip_special_tokens=True).strip()

        if grounded(text, r['context']):
            n_ground += 1
        tot_f1 += f1(text, r['answer'])
        if r['kind'] == 'refusal':
            n_ref += 1
            if text.lower().startswith('not in the archive'):
                n_ref_ok += 1
        if len(samples) < 4:
            samples.append((r['question'], text))

    n = min(limit, len(rows))
    return {
        'grounded': n_ground / n,
        'f1': tot_f1 / n,
        'refusal': (n_ref_ok / n_ref) if n_ref else float('nan'),
        'n': n, 'n_ref': n_ref, 'samples': samples,
    }


def main():
    holdout, base, tuned = sys.argv[1], sys.argv[2], sys.argv[3]
    limit = int(sys.argv[4]) if len(sys.argv) > 4 else 120
    dev = 'mps' if torch.backends.mps.is_available() else 'cpu'
    rows = json.load(open(holdout))

    print('%d held-out examples, evaluating %d on %s\n' % (len(rows), limit, dev))
    for name, path in (('base (SmolLM2-135M)', base), ('fine-tuned', tuned)):
        r = run(path, rows, dev, limit)
        print('%-22s grounded %5.1f%%   refusal %5.1f%%   token-F1 %.3f'
              % (name, 100 * r['grounded'],
                 100 * r['refusal'] if r['refusal'] == r['refusal'] else 0,
                 r['f1']))
        for q, a in r['samples']:
            print('    Q %s' % q[:70])
            print('    A %s' % a[:140].replace('\n', ' '))
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
