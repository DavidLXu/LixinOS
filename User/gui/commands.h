#ifndef LX_COMMANDS_H
#define LX_COMMANDS_H
/* The table index identifies the action for every menu and keyboard entry point. */
typedef struct { const char *label; int key, requirement; } UiCommand;
static inline int ui_command_for_key(const UiCommand *commands,int count,int key) {
    for (int i=0;key>0 && i<count;++i)
        if (commands[i].key==key) return i;
    return -1;
}
#endif
