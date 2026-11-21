#pragma once

#define AGENT_SESSION_LIMIT 12
#define AGENT_TRACE_NAME_LIMIT 64

/* Adapted from the guest Agent's prototype. lx_dir returns negative sentinels
 * for both directories and the end of enumeration. */
static int scan_sessions(const char *root, char names[AGENT_SESSION_LIMIT][AGENT_TRACE_NAME_LIMIT]) {
    int count = 0;
    for (unsigned index = 0;; index++) {
        char name[513];
        long size = lx_dir(root,index,name);
        if (size == LX_END || size < LX_DIR) break;
        if (size == LX_DIR) continue;
        unsigned long length = strlen(name);
        if (length != sizeof(".agent-trace-0000000000-0000.jsonl")-1 || strcmp(name+length-6,".jsonl")) continue;
        const char *prefix = ".agent-trace-";
        int same = 1;
        for (unsigned i=0;prefix[i];i++) if (name[i]!=prefix[i]) same=0;
        if (!same) continue;
        int at=0;
        while (at<count && strcmp(names[at],name)>0) ++at;
        if (at==AGENT_SESSION_LIMIT) continue;
        if (count<AGENT_SESSION_LIMIT) ++count;
        for (int i=count-1;i>at;--i) copy(names[i],names[i-1],AGENT_TRACE_NAME_LIMIT);
        copy(names[at],name,AGENT_TRACE_NAME_LIMIT);
    }
    return count;
}
