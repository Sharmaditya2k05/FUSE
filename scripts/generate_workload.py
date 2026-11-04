#!/usr/bin/env python3
"""Print a reproducible block-number trace, one block per line.

Example:
    python3 scripts/generate_workload.py --kind clustered --count 32 --seed 7
"""
import argparse
import random


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--kind", choices=["sequential", "random", "clustered", "mixed"],
                        default="random")
    parser.add_argument("--count", type=int, default=64, help="number of requests")
    parser.add_argument("--blocks", type=int, default=4096, help="disk size in blocks")
    parser.add_argument("--seed", type=int, default=312, help="RNG seed (same seed, same trace)")
    args = parser.parse_args()
    rng = random.Random(args.seed)

    if args.kind == "sequential":
        data = list(range(10, 10 + args.count))
    elif args.kind == "random":
        data = [rng.randrange(args.blocks) for _ in range(args.count)]
    elif args.kind == "clustered":
        # Requests fall within +/-16 blocks of one randomly chosen centre.
        center = rng.randrange(args.blocks)
        data = [max(0, min(args.blocks - 1, center + rng.randrange(-16, 17)))
                for _ in range(args.count)]
    else:
        # Mostly sequential, with every eighth request jumping somewhere random.
        data = [10 + i if i % 8 else rng.randrange(args.blocks) for i in range(args.count)]

    print("\n".join(map(str, data)))


if __name__ == "__main__":
    main()
