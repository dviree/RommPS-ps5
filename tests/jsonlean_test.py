#!/usr/bin/env python3
"""Random JSON through the streaming filter (src/jsonlean.c), against Python's
own idea of the same JSON without the dropped members.

  jsonlean_test.py CLI [CASES]
"""
import json
import random
import subprocess
import sys

DROP = ["merged_ra_metadata", "ss_metadata", "x"]
NAMES = DROP + ["name", "fs_name", "files", "a\"b", "k" * 70, "Pokémon", "merged_ra_metadata_not", "y"]


def value(depth):
    r = random.random()
    if depth > 4 or r < 0.35:
        return random.choice([0, -12.5e3, True, False, None, "", "x", "merged_ra_metadata",
                              'q"uo\\te', "{[,:]}", "Luigi’s", "\n\t"])
    if r < 0.7:
        return {random.choice(NAMES): value(depth + 1) for _ in range(random.randint(0, 5))}
    return [value(depth + 1) for _ in range(random.randint(0, 4))]


def strip(v):
    if isinstance(v, dict):
        return {k: strip(x) for k, x in v.items() if k not in DROP}
    if isinstance(v, list):
        return [strip(x) for x in v]
    return v


def main():
    cli, cases = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 3000
    random.seed(1)
    for i in range(cases):
        doc = value(0)
        text = json.dumps(doc, ensure_ascii=random.random() < 0.5,
                          indent=random.choice([None, None, 1]),
                          separators=random.choice([(",", ":"), (", ", ": ")]))
        got = subprocess.run([cli, str(i)] + DROP, input=text.encode(), capture_output=True)
        if got.returncode:
            sys.exit(f"case {i}: exit {got.returncode}\n{text}")
        try:
            out = json.loads(got.stdout)
        except ValueError as e:
            sys.exit(f"case {i}: not JSON ({e})\nin:  {text}\nout: {got.stdout.decode(errors='replace')}")
        if out != strip(doc):
            sys.exit(f"case {i}: wrong result\nin:  {text}\nout: {got.stdout.decode()}")
    print(f"jsonlean: ok ({cases} cases)")


if __name__ == "__main__":
    main()
