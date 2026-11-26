"""Build the real retail PE loader without claiming any translated game functions."""
import os
from pathlib import Path
import sys

kit,project,out = map(Path,sys.argv[1:4])
target=sys.argv[4]
os.environ["PCRECOMP"]=str(kit.resolve())
sys.path.insert(0,str(project))
import run_lift
out.mkdir(parents=True,exist_ok=True)
run_lift.write_target_header(str(out),target,run_lift.targets()[target])
(out/"recomp_dispatch.c").write_text('''#include "recomp_types.h"
const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0,0}};
const uint32_t recomp_dispatch_count = 0;
const uint32_t ra2_entry_va = 0;
''')
# Upstream's dry-run path loads and binds the retail PE before entering the lift.
src=(project/"src/linux/main.c").read_text()
begin=src.index("    if (!run) {\n",src.index("int unresolved = recomp_pe_bind"))
end=src.index("    if (chdir(game_full)",begin)
src=src[:begin]+'''    printf("Retail image mapped and bound at entry 0x%08X.\\n", img.entry);
    puts("Build the game translation with RA2_DIR before running this image.");
    return run ? 2 : 0;
'''+src[end:]
(out/"preflight-main.c").write_text(src)
