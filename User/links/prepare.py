"""Configure the upstream text engine for LixinOS native file/TCP transport."""
from pathlib import Path
import re
import sys

src, out = map(Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
def source(name):
    return (src/name).read_text(encoding="latin1")
def write(name, text):
    (out/name).write_text(text, encoding="latin1")

header = source("links.h").replace("#include <netdb.h>", "struct sockaddr;").replace(
    "#include <sys/socket.h>", "").replace("#include <netinet/in.h>", "")
write("links.h", header)
for name in ("html", "html_r", "html_tbl", "charsets", "string", "memory", "view", "jsint", "types"):
    write(name+".c", source(name+".c"))
# Retain the URL grammar and relative-link resolver. Transport is dispatched in port.c.
url = source("url.c")
a,b=url.index("} protocols[]="),url.index("static int check_protocol")
write("url.c",url[:a]+re.sub(r"\b\w+_func\b","NULL",url[a:b])+url[b:])
http = source("http.c")
write("headers.c", '#include "links.h"\n'+http[http.index("unsigned char *parse_http_header"):http.index("static const unsigned char *months")])

# Compile only the upstream helpers needed by the text renderer. The full UI
# dispatches POSIX menu and asynchronous loader callbacks unavailable here.
def function(name, marker):
    text = source(name)
    start = text.index(marker)
    end = text.index("\n}\n", start) + 3
    return text[start:end]
view = source("view.c")
no_js = view.index("void free_js_event_spec", view.index("void free_js_event_spec") + 1)
write("view.c", '#include "links.h"\n' + view[no_js:view.index("#endif",no_js)]
      + function("view.c","LIBC_CALLBACK static int comp_links(")+function("view.c","void sort_links("))
html = source("html.c")
start = html.index("static void html_link(")
# Ignore speculative HTTP requests; navigation always loads the selected URL.
html = html.replace("if (!dmp) load_url(pre_url, format_.href_base, NULL, PRI_PRELOAD, NC_ALWAYS_CACHE, 0, 0, 0, 0);","")
a=html.index('\t\tunsigned char *pre_url, *host;', start)
b=html.index('\n\t\tgoto skip;\n\t}',a)
html=html[:a]+html[b:]
a=html.index("void do_select_submenu(")
b=html.index("\n}\n",a)
html=html[:html.index("{",a)]+"{ /* Forms are read-only in the native viewer. */"+html[b:]
write("html.c",html)
write("helpers.c",'#include "links.h"\n'+function("terminal.c","unsigned char get_attribute(")
      +function("sched.c","int is_proxy_url(")+function("error.c","unsigned char *memacpy(")
      +function("error.c","unsigned char *stracpy("))

# Reserve two cells per Han glyph. Keep the continuation cell out of link
# text output and permit a line break immediately before a full-width glyph.
renderer = '#include "unicode.h"\n' + source("html_r.c")
renderer = renderer.replace("uni_c[ll] = un;", "if (han_slot(un)>=0) { uni_c[ll++]=un; un=WIDE_TAIL; }\n\t\t\t\t\tuni_c[ll] = un;")
a = renderer.index("static inline void set_hline_uni")
renderer = renderer[:a] + renderer[a:].replace("p->spaces[x] = *d == ' ';",
    "p->spaces[x] = (*d == ' ') ? 1 : (han_slot(*d)>=0 ? 2 : 0);", 1)
a = renderer.index("static int split_line(")
b = renderer.index("static inline void compress_line", a)
split = renderer[a:b].replace("int i;", "int i, skip;")
split = split.replace("&& p->spaces[i])", "&& p->spaces[i] && (p->spaces[i]==1 || i>par_format.leftmargin))")
split = split.replace("split:\n", "split:\n\tskip = p->spaces[i] == 1;\n")
split = split.replace("safe_add(i,1)", "safe_add(i,skip)").replace("safe_add(i, 1)", "safe_add(i, skip)")
split = split.replace("p->spl - i - 1", "p->spl - i - skip").replace("0, i + 1", "0, i + skip").replace("leftmargin + 1;", "leftmargin + skip;")
write("html_r.c", renderer[:a] + split + renderer[b:])
