"""Allocation-heavy, deterministic Python workload for tools/preload_demo.sh.

Covers the interpreter's object churn (dicts, lists, strings, big ints), C
extensions (json, re, hashlib, zlib), threads, and fork() from a process with
live threads (multiprocessing's "fork" start method, D-20). Output must be
byte-identical with and without libmemloc.so.
"""

import hashlib
import json
import multiprocessing
import re
import threading
import zlib


def digest(obj):
    return hashlib.sha256(repr(obj).encode()).hexdigest()[:16]


def churn(n):
    table = {f"key-{i}": [i, str(i) * (i % 17), {"v": i * i}] for i in range(n)}
    text = json.dumps(table, sort_keys=True)
    back = json.loads(text)
    words = re.findall(r"key-(\d+)", text)
    packed = zlib.compress(text.encode())
    return len(back), len(words), len(packed) > 0, digest(sorted(back)[:100])


def square_sum(k):
    return sum(i * i for i in range(k)) + len(str(3 ** (k % 5000)))


def threaded(count):
    results = [None] * count

    def work(slot):
        parts = ["-".join(str(j) for j in range(slot, slot + 2000)) for _ in range(50)]
        results[slot] = digest(parts)

    threads = [threading.Thread(target=work, args=(i,)) for i in range(count)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return digest(results)


def main():
    print("churn", churn(200_000))
    print("threads", threaded(8))
    # Fork while this process has threads allocating in the background.
    background = threading.Thread(target=churn, args=(100_000,))
    background.start()
    with multiprocessing.get_context("fork").Pool(4) as pool:
        print("fork-pool", sum(pool.map(square_sum, range(0, 20000, 50))))
    background.join()
    print("bigint", digest(hex(3 ** 200_000)))


if __name__ == "__main__":
    main()
