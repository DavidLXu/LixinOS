"""Add V8's upstream ARM64 DrumBrake sources to Node 26's GYP build."""
from pathlib import Path
import sys

src = Path(sys.argv[1])
gyp = src / "tools/v8_gypfiles/v8.gyp"
features = gyp.with_name("features.gypi")
text = features.read_text()
old = "'defines': ['V8_ENABLE_WEBASSEMBLY',],"
new = "'defines': ['V8_ENABLE_WEBASSEMBLY', 'V8_ENABLE_DRUMBRAKE', 'V8_DRUMBRAKE_BOUNDS_CHECKS',],"
if old in text:
    features.write_text(text.replace(old, new))
elif new not in text:
    raise ValueError("unknown V8 feature definitions")

text = gyp.read_text()
sources = {
    "v8_initializers": ["arm64/interpreter-builtins-arm64.cc"],
    "v8_base_without_compiler": ["wasm-interpreter-objects.cc", "wasm-interpreter-runtime.cc",
                                 "wasm-interpreter-simd.cc", "wasm-interpreter.cc"],
}
for target, files in sources.items():
    names = [f"src/wasm/interpreter/{name}" for name in files]
    if all(name in text for name in names):
        continue
    start = text.index(f"'target_name': '{target}'")
    when = text.index("['v8_enable_webassembly==1'", start)
    pos = text.index("'sources': [", when) + len("'sources': [")
    for name in names:
        if not (src / "deps/v8" / name).is_file():
            raise FileNotFoundError(name)
    text = text[:pos] + "".join(f"\n            '<(V8_ROOT)/{name}'," for name in names) + text[pos:]
if text != gyp.read_text():
    gyp.write_text(text)

# GCC rejects the five explicit member specializations inside the class.
# Retain the templated entry point and delegate to ordinary overloads.
header = src / "deps/v8/src/wasm/interpreter/wasm-interpreter.h"
text = header.read_text()
start = text.index("  template <typename T>\n  inline std::optional<uint32_t> GetConstSlot(T value)")
end = text.index("  template <typename T>\n  inline void InsertConstSlotInCache", start)
block = text[start:end]
if "GetTypedConstSlot" not in block:
    import re
    block = block.replace("    UNREACHABLE();", "    return GetTypedConstSlot(value);", 1)
    block, count = re.subn(r"  template <>\n  inline std::optional<uint32_t> GetConstSlot<([^>]+)>",
                           "  inline std::optional<uint32_t> GetTypedConstSlot", block)
    if count != 5:
        raise ValueError("unknown V8 constant cache layout")
    header.write_text(text[:start] + block + text[end:])

# Node's V8 snapshot uses WireBytesRef for passive data segments. Match the
# engine's memory_init_wrapper / data_drop_wrapper and preserve range checks.
interpreter = src / "deps/v8/src/wasm/interpreter"
def replace_once(file, old, new):
    data = file.read_text()
    if old in data:
        if data.count(old) != 1:
            raise ValueError(f"ambiguous input in {file.name}")
        file.write_text(data.replace(old, new))
    elif new not in data:
        raise ValueError(f"unexpected input in {file.name}")


replace_once(interpreter/"wasm-interpreter-runtime-inl.h",
             "data_segment_sizes()->set(index, 0)", "data_segments()->set(index, WireBytesRef{})")
runtime = interpreter/"wasm-interpreter-runtime.cc"
replace_once(runtime, "uint64_t src_max =\n      trusted_data->data_segment_sizes()->get(data_segment_index);",
             "WireBytesRef segment = trusted_data->data_segments()->get(data_segment_index);\n"
             "  uint64_t src_max = segment.length();")
replace_once(runtime, "Address src_addr =\n      trusted_data->data_segment_starts()->get(data_segment_index) + src;",
             "const uint8_t* src_addr = trusted_data->native_module()->wire_bytes().data() + segment.offset() + src;")
replace_once(runtime, "const uint8_t* src_addr = trusted_data->native_module()->wire_bytes().data() + segment.offset() + src;\n  std::memmove(reinterpret_cast<void*>(dst_addr),\n               reinterpret_cast<void*>(src_addr), size);",
             "const uint8_t* src_addr = trusted_data->native_module()->wire_bytes().data() + segment.offset() + src;\n  std::memmove(reinterpret_cast<void*>(dst_addr), src_addr, size);")

# GCC requires namespace-scope explicit specializations. Equivalent constexpr
# branches keep the reference stack and the raw value stack distinct.
cc = interpreter/"wasm-interpreter.cc"
data = cc.read_text()
if "inline WasmRef pop(" in data:
    start = data.index("  template <>\n  slot_offset_t Read<slot_offset_t>")
    end = data.index("  // Returns the maximum", start)
    data = data[:start] + data[end:]
    for name, typ in (("push", "void"), ("pop", "WasmRef")):
        spec = data.index(f"  template <>\n  inline {typ} {name}(")
        end = data.index("\n  }",spec)+4
        start = data.rindex("  template <typename T>",0,spec)
        generic = data[start:spec].rstrip()
        head, normal = generic.split(" {\n",1)
        body = data[spec:end].split(" {\n",1)[1].rsplit("\n  }",1)[0]
        if name == "push":
            body=body.replace("ref_stack_index, ref)", "ref_stack_index, val)")
        data = data[:start] + head + " {\n    if constexpr (std::is_same_v<T, WasmRef>) {\n" + body + "\n    } else {\n" + normal.rsplit("\n  }",1)[0] + "\n    }\n  }" + data[end:]
    cc.write_text(data)
