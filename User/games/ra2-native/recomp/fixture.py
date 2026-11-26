"""Make a small owned PE and lift its one arithmetic function with pcrecomp."""
from pathlib import Path
import struct
import sys
import os
kit, upstream, out = map(Path, sys.argv[1:])
os.environ["PCRECOMP"] = str(kit.resolve())
sys.path.insert(0, str(upstream))
import run_lift as lift
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
out.mkdir(parents=True, exist_ok=True)
# mov eax,6; imul eax,eax,7; ret
code=bytes.fromhex("b8060000006bc007c3")
# Place the entry beyond the first 4 KiB read, as in a retail executable.
raw_offset = 8192
image=bytearray(raw_offset + 512)
image[:2]=b'MZ'
struct.pack_into('<I',image,0x3c,0x80)
image[0x80:0x84]=b'PE\0\0'
stamp=0x4c585241
struct.pack_into('<HHIIIHH',image,0x84,0x14c,1,stamp,0,0,224,0x102)
opt=0x98
struct.pack_into('<H',image,opt,0x10b)
for offset,value in {4:512,16:4096,20:4096,28:0x400000,32:4096,36:512,56:8192,60:512,72:1048576,76:4096,80:1048576,84:4096,92:16}.items():
    struct.pack_into('<I',image,opt+offset,value)
struct.pack_into('<H',image,opt+68,3)
struct.pack_into('<8sIIIIIIHHI',image,opt+224,b'.text',len(code),4096,512,raw_offset,0,0,0,0,0x60000020)
image[raw_offset:raw_offset+len(code)]=code
(out/'fixture.exe').write_bytes(image)
entry=0x401000
cs=Cs(CS_ARCH_X86,CS_MODE_32)
cs.detail=True
insns,leaders=lift.linear_disassemble_function(cs,code,entry,entry,entry+len(code))
name='ra2_test_arithmetic'
body=lift.lift_function_linear(lift.Lifter(),name,insns,leaders,entry)
lift.write_chunk(str(out),0,[(body,entry,name)])
(out/'recomp_funcs.h').write_text('void ra2_test_arithmetic(void);\n')
(out/'recomp_dispatch.c').write_text('''#include "recomp_types.h"
#include "recomp_funcs.h"
const recomp_dispatch_entry_t recomp_dispatch_table[]={{0x401000,ra2_test_arithmetic}};
const uint32_t recomp_dispatch_count=1;
const uint32_t ra2_entry_va=0x401000;
''')
metadata=dict(lift.targets()['game'],title='LixinOS recompilation test',exe_name='fixture.exe',
              ini_name='fixture.ini',stamp=stamp,game_frame=0x401080,hooks={})
lift.write_target_header(str(out),'game',metadata)
