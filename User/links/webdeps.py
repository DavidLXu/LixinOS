"""Prepare pinned browser libraries and a fixed-width Chinese font for the guest."""
from pathlib import Path
import gzip
import hashlib
import re
import subprocess
import sys
import tarfile
import tempfile

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)


def fetch(name, url, digest):
    path = out / name
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == digest:
        return path
    with tempfile.NamedTemporaryFile(dir=out) as download:
        subprocess.run(["curl", "-fsSL", "--retry", "2", url, "-o", download.name], check=True)
        data = Path(download.name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"Wrong digest for {name}")
        path.write_bytes(data)
    return path


archive = fetch("duktape.tar.xz", "https://sources.buildroot.net/duktape/duktape-2.7.0.tar.xz",
                "90f8d2fa8b5567c6899830ddef2c03f3c27960b11aca222fa17aa7ac613c2890")
font = fetch("unifont.hex.gz", "https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.hex.gz",
             "f9c8c7802453f47be02677176aeac2342ee96d354fad7a26cedcce48e68e1d9f")
with tarfile.open(archive) as src:
    for name in ("duktape.c", "duktape.h", "duk_config.h"):
        data = src.extractfile("duktape-2.7.0/src/" + name).read().decode()
        if name == "duk_config.h":
            data = data.replace("#undef DUK_USE_INTERRUPT_COUNTER", "#define DUK_USE_INTERRUPT_COUNTER")
            data = data.replace("#undef DUK_USE_EXEC_TIMEOUT_CHECK", "extern int script_timeout(void *);\n#define DUK_USE_EXEC_TIMEOUT_CHECK(udata) script_timeout(udata)")
            data = data.replace("#undef DUK_USE_DATE_GET_NOW", "extern double script_time(void);\n#define DUK_USE_DATE_GET_NOW(ctx) script_time()")
            data = data.replace("#undef DUK_USE_DATE_GET_LOCAL_TZOFFSET", "#define DUK_USE_DATE_GET_LOCAL_TZOFFSET(d) 0")
            data = re.sub(r"(#define DUK_USE_\w+_RECLIMIT) \d+", r"\g<1> 32", data)
            data = data.replace("#define DUK_USE_COMPILER_RECLIMIT 32", "#define DUK_USE_COMPILER_RECLIMIT 256")
            for name, depth in (("REGEXP_EXECUTOR",1024),("NATIVE_CALL",128)):
                data = data.replace(f"#define DUK_USE_{name}_RECLIMIT 32",f"#define DUK_USE_{name}_RECLIMIT {depth}")
            data = data.replace("#undef DUK_USE_NATIVE_STACK_CHECK", "extern int script_stack_low(void);\n#define DUK_USE_NATIVE_STACK_CHECK() script_stack_low()")
        (out / name).write_text(data)

# U+3000..9FFF, compatibility ideographs, and fullwidth punctuation.
atlas = bytearray((0x7000 + 0x200 + 0x100)*32)
for line in gzip.open(font, "rt"):
    code, bits = line.strip().split(":")
    ch = int(code, 16)
    if 0x3000 <= ch < 0xa000:
        slot = ch - 0x3000
    elif 0xf900 <= ch < 0xfb00:
        slot = 0x7000 + ch - 0xf900
    elif 0xff00 <= ch < 0x10000:
        slot = 0x7200 + ch - 0xff00
    else:
        continue
    glyph = bytes.fromhex(bits)
    if len(glyph) == 32:
        atlas[32*slot:32*slot+32] = glyph
(out / "han.font").write_bytes(atlas)

# Bundle the CommonJS modules for evaluation by the guest's Duktape runtime.
domino = fetch("domino.tgz", "https://registry.npmjs.org/domino/-/domino-2.1.6.tgz",
               "3343d0037ff12e486cf64a6eb5455f272fbba205fcc967882d86bb24ee6df159")
with tarfile.open(domino) as src, (out / "dom.js").open("w") as bundle:
    bundle.write("(function(global){var modules=Object.create(null),cache=Object.create(null);\n")
    for entry in src.getmembers():
        if not re.fullmatch(r"package/lib/[\w.-]+\.js", entry.name):
            continue
        bundle.write(f"modules['./{Path(entry.name).stem}']=function(module,exports,require){{\n")
        code=src.extractfile(entry).read().decode()
        if entry.name.endswith('/HTMLParser.js'):
            # Use the same entity table and longest-match rule without backtracking
            # through thousands of alternatives on the guest's native stack.
            code=re.sub(r'^var NAMEDCHARREF = .*?;$','',code,flags=re.M)
            start=code.index('    NAMEDCHARREF.lastIndex = nextchar;')
            end=code.index('    if (!name)',start)
            code=code[:start]+"""    var name = '', size=Math.min(chars.length-nextchar,NAMEDCHARREF_MAXLEN);
    for (; size>0; size--) {
      var key=chars.substr(nextchar,size);
      if (Object.prototype.hasOwnProperty.call(namedCharRefs,key)) {name=key; break;}
    }
"""+code[end:]
            # Document.close dispatches load after DOMContentLoaded in the guest.
            code=code.replace('doc.defaultView.dispatchEvent(new impl.Event("load",{}));','')
            # The large legacy doctype expression only applies to a public identifier.
            code=re.sub(r'(\w+PublicIds)\.test\(publicid\)',r'(publicid && \1.test(publicid))',code)
        if entry.name.endswith('NodeList.es6.js'):
            code='module.exports=require("./NodeList.es5");'
        bundle.write(code)
        bundle.write("\n};\n")
    bundle.write("""function require(name) {
        if (name.slice(-3) === ".js") name=name.slice(0,-3);
        if (!cache[name]) {
            var m=cache[name]={exports:{}};
            modules[name](m,m.exports,require);
        }
        return cache[name].exports;
    }
    return require('./impl');})(this)""")
