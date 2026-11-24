"""Run all orders on the host, print a reproducible CSV to stdout."""
import argparse
import csv
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generations", type=int, default=300)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--orders", type=int, nargs="+", choices=range(3, 11),
                        default=range(3, 11))
    args = parser.parse_args()
    if min(args.generations, args.jobs) < 1:
        parser.error("generations and jobs must be positive")
    source = Path(__file__).with_name("swing-orders.c")
    seeds = (0x3AB97C51, 0x91C706AD, 0x2E0FB349)
    tasks = [(order, seed) for seed in seeds for order in args.orders]
    with tempfile.TemporaryDirectory() as temp:
        exe = Path(temp) / "swing-orders"
        subprocess.run([os.environ.get("CC", "cc"), "-O3", "-std=c11",
                        source, "-lm", "-o", exe], check=True)

        def train(task):
            order, seed = task
            raw = subprocess.check_output([exe, str(order), str(seed),
                                           str(args.generations)], text=True)
            return next(csv.reader([raw.strip()]))

        with ThreadPoolExecutor(args.jobs) as pool:
            rows = list(pool.map(train, tasks))

    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(["order", "seed", "budget_generations", "passed_validation",
                     "generations_run", "candidate_episodes", "wall_seconds",
                     "validation_successes_of_16", "test_successes_of_32",
                     "nominal_final_hold_seconds"])
    writer.writerows(sorted(rows, key=lambda row: (int(row[0]), int(row[1]))))


if __name__ == "__main__":
    main()
