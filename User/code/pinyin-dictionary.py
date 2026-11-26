"""Prepare ranked pinyin candidates from the pinned Rime simplified dictionary."""
from collections import defaultdict
from pathlib import Path
import sys
import tarfile

archive, destination = map(Path, sys.argv[1:])
groups = defaultdict(dict)
with tarfile.open(archive) as source:
    for entry in source:
        name = Path(entry.name).name
        if name in ("AUTHORS", "LICENSE"):
            (destination / ("pinyin-" + name)).write_bytes(source.extractfile(entry).read())
        elif name == "pinyin_simp.dict.yaml":
            for line in source.extractfile(entry).read().decode().splitlines():
                parts = line.split("\t")
                if len(parts) != 3 or not parts[2].isdigit():
                    continue
                word, spelling, weight = parts
                candidates = groups[spelling.replace(" ", "")]
                candidates[word] = max(candidates.get(word, 0), int(weight))
assert "你好" in groups["nihao"]
with (destination / "pinyin.tsv").open("w") as out:
    for key, candidates in sorted(groups.items()):
        print(key, *sorted(candidates, key=candidates.get, reverse=True), sep="\t", file=out)
