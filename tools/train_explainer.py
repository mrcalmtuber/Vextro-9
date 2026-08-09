#!/usr/bin/env python3
"""Fine-tune the grounded-answer model.

Takes SmolLM2-135M-Instruct and teaches it one job: answer a question
from a Wikipedia passage, in the passage's own words, and say "Not in the
archive." when the passage does not answer it.

Not trained from scratch, and the reason is arithmetic. A 200M model
wants something like four billion tokens to be worth its size; Simple
English Wikipedia is about three hundred million, and a properly-fed run
would be days of continuous compute on this machine. A model trained
under those conditions would be weaker than the 0.5B it was meant to
improve on. Fine-tuning a well-trained small model on the exact task is
the version of this that works.

The loss is computed on the answer only. The question and the passage are
context, not something to learn to reproduce -- training on them teaches
the model to continue Wikipedia articles, which is what it already does
and precisely the habit being trained out.

    build/venv/bin/python tools/train_explainer.py \\
        build/dataset.jsonl build/smol-base build/explainer
"""
import json
import math
import random
import sys
import time

import torch
from torch.utils.data import Dataset, DataLoader
from transformers import AutoModelForCausalLM, AutoTokenizer

MAX_LEN = 512
SYSTEM = ("You answer only from the Context. Do not use any other "
          "knowledge. If the Context does not contain the answer, reply "
          "exactly: Not in the archive. Be brief.")


def build_prompt(tok, question, context):
    """The same ChatML the kernel sends. SmolLM2 and Qwen2 share it,
    which is why one prompt builder serves both models."""
    return (
        "<|im_start|>system\n" + SYSTEM + "<|im_end|>\n"
        "<|im_start|>user\nContext: " + context + "\n"
        "Question: " + question + "<|im_end|>\n"
        "<|im_start|>assistant\n")


class Grounded(Dataset):
    def __init__(self, rows, tok):
        self.rows = rows
        self.tok = tok

    def __len__(self):
        return len(self.rows)

    def __getitem__(self, i):
        r = self.rows[i]
        prompt = build_prompt(self.tok, r['question'], r['context'])
        answer = r['answer'] + "<|im_end|>"

        p_ids = self.tok(prompt, add_special_tokens=False)['input_ids']
        a_ids = self.tok(answer, add_special_tokens=False)['input_ids']

        # Trim the passage, not the answer: an example whose answer has
        # been cut teaches the model to stop mid-sentence.
        room = MAX_LEN - len(a_ids)
        if room < 16:
            a_ids = a_ids[:MAX_LEN - 16]
            room = 16
        if len(p_ids) > room:
            p_ids = p_ids[:room]

        ids = p_ids + a_ids
        labels = [-100] * len(p_ids) + a_ids[:]     # loss on the answer only
        return {'input_ids': ids, 'labels': labels}


def collate(batch, pad_id):
    n = max(len(b['input_ids']) for b in batch)
    ids, labels, mask = [], [], []
    for b in batch:
        k = n - len(b['input_ids'])
        ids.append(b['input_ids'] + [pad_id] * k)
        labels.append(b['labels'] + [-100] * k)
        mask.append([1] * len(b['input_ids']) + [0] * k)
    return (torch.tensor(ids), torch.tensor(labels), torch.tensor(mask))


def main():
    data_path, base, out = sys.argv[1], sys.argv[2], sys.argv[3]
    epochs = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0

    dev = 'mps' if torch.backends.mps.is_available() else 'cpu'
    print('device:', dev)

    tok = AutoTokenizer.from_pretrained(base)
    if tok.pad_token_id is None:
        tok.pad_token = tok.eos_token
    model = AutoModelForCausalLM.from_pretrained(base, dtype=torch.float32)
    model.to(dev)
    model.gradient_checkpointing_enable()
    model.train()

    rows = [json.loads(l) for l in open(data_path)]
    random.Random(11).shuffle(rows)
    hold = rows[:400]
    rows = rows[400:]
    json.dump(hold, open(out + '.holdout.json', 'w'))
    print('%d training, %d held out' % (len(rows), len(hold)))

    ds = Grounded(rows, tok)
    dl = DataLoader(ds, batch_size=8, shuffle=True,
                    collate_fn=lambda b: collate(b, tok.pad_token_id))

    steps = int(len(dl) * epochs)
    opt = torch.optim.AdamW(model.parameters(), lr=3e-5, weight_decay=0.01)
    sched = torch.optim.lr_scheduler.OneCycleLR(
        opt, max_lr=3e-5, total_steps=steps, pct_start=0.03)

    t0 = time.time()
    step, run = 0, 0.0
    done = False
    while not done:
        for ids, labels, mask in dl:
            ids, labels, mask = ids.to(dev), labels.to(dev), mask.to(dev)
            outp = model(input_ids=ids, attention_mask=mask, labels=labels)
            loss = outp.loss
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            sched.step()
            opt.zero_grad(set_to_none=True)

            run += loss.item()
            step += 1
            if step % 25 == 0:
                el = time.time() - t0
                per = el / step
                print('step %5d/%d  loss %.4f  ppl %.2f  %.2fs/step  eta %.0fm'
                      % (step, steps, run / 25, math.exp(min(run / 25, 20)),
                         per, (steps - step) * per / 60), flush=True)
                run = 0.0
            if step >= steps:
                done = True
                break

    model.save_pretrained(out)
    tok.save_pretrained(out)
    print('saved to %s after %.1f minutes' % (out, (time.time() - t0) / 60))
    return 0


if __name__ == '__main__':
    sys.exit(main())
