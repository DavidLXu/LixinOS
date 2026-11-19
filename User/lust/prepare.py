"""Adapt the pinned, single-file upstream pipeline for alloc + core on EL0."""
import re
import sys
from pathlib import Path

source, dest = map(Path, sys.argv[1:])
dest.mkdir(parents=True, exist_ok=True)
modules = "ast borrowck cgen check collect diag intern ir lexer lower parser types".split()
for name in modules:
    s = (source / (name + ".rs")).read_text()
    s = s.replace("std::collections::HashMap", "alloc::collections::BTreeMap")
    s = s.replace("use std::collections::HashSet;", "use alloc::collections::BTreeSet as HashSet;")
    s = s.replace("use std::collections::{HashMap, HashSet};",
                  "use alloc::collections::{BTreeMap as HashMap, BTreeSet as HashSet};")
    s = s.replace("Eq, Hash)]", "Eq, Hash, PartialOrd, Ord)]")
    if name == "intern":
        s = s.replace("BuildHasherDefault, ", "").replace("HashMap<K, V, BuildHasherDefault<FxHasher>>", "alloc::collections::BTreeMap<K, V>")
        s = s.replace("use std::sync::Mutex;", "use core::cell::RefCell as Mutex;").replace(".lock().unwrap()", ".borrow_mut()")
    if name == "ast":
        start = s.index("        std::thread::scope")
        end = s.index("        let mut out", start)
        s = s[:start] + "        for (c, &shift) in chunks.iter_mut().zip(&shifts).skip(1) { c.shift(shift); }\n" + s[end:]
    if name == "cgen":
        start = s.index("fn unit_header()")
        end = s.index("/// Unit 0:", start)
        s = s[:start] + 'fn unit_header() -> String { include_str!("runtime.c").into() }\n' + s[end:]
        s += "\n" + (Path(__file__).parent / "standalone.rs").read_text()
    for module in ("mem", "fmt", "iter", "hash", "ops"):
        s = s.replace(f"std::{module}::", f"core::{module}::")
    (dest / (name + ".rs")).write_text("use crate::prelude::*;\n" + s)

runtime = (source / "runtime.c").read_text()
for header in ("stdlib.h", "string.h", "stdio.h"):
    runtime = runtime.replace(f"#include <{header}>", "")
runtime = runtime.replace("#include <stdint.h>\n#include <stddef.h>", (Path(__file__).parent / "runtime.h").read_text())
# TinyCC emits static inline helpers only when used. Keep global storage in
# a single unit, while each code unit sees the runtime function definitions.
runtime = runtime.replace("#ifdef LUST_IMPL", "#if 1", 1)
runtime = re.sub(r"^(?!#)([^\s/][^\n]*? LUST_BODY\()", r"static inline \1", runtime, flags=re.M)
runtime = runtime.replace("#define LUST_ABS(T, N) T lust_abs_", "#define LUST_ABS(T, N) static inline T lust_abs_")
(dest / "runtime.c").write_text(runtime)
