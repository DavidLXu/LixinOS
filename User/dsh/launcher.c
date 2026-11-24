#define LX_SCRIPT_PREFIX ".\0/bin/node\0/bin/node\0--max-old-space-size=128\0--expose-internals\0--require\0/lib/dsh/fetch.cjs\0/lib/dsh/lib/bin.js"
#define LX_SCRIPT_ERROR "dsh: cannot launch /bin/node\n"
#include "../node/script-launcher.h"
