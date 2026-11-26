/* App names may come from the installed filesystem as well as the catalogue. */
#include "launcher-pinyin.h"

static const LauncherPinyin *app_dictionary(void) {
#ifdef LX_NETSURF
    static LauncherPinyin *table;
    static int tried;
    if (tried) return table;
    tried=1;
    const char *path="/lib/netsurf/launcher-pinyin";
    if (lx_stat(path)!=sizeof *table) return NULL;
    int fd=lx_open(path);
    if (fd<0) return NULL;
    table=malloc(sizeof *table);
    size_t used=0;
    while (table && used<sizeof *table) {
        int n=lx_read(fd,(char *)table+used,sizeof *table-used);
        if (n<=0) break;
        used+=n;
    }
    lx_close(fd);
    if (used!=sizeof *table) { free(table); table=NULL; }
    return table;
#else
    return &launcher_dictionary;
#endif
}

static void normalize_query(const char *query,char *out) {
    while (*query) {
        unsigned char c=*query++;
        if (c!=' ' && c!='-' && c!='\'') *out++=fold_ascii(c);
    }
    *out=0;
}

static int matches_title(const char *title,const char *query,const char *spelling) {
    if (name_matches(title,query)) return 1;
    if (!*spelling || (unsigned char)*spelling>=128) return 0;
    /* A guest filename is at most 512 UTF-8 bytes. */
    char full[1536], initials[513];
    int length=0,n=0;
    const LauncherPinyin *dict=app_dictionary();
    while (*title) {
        unsigned cp=utf8_next(&title);
        unsigned index=dict && cp>=0x3400 && cp<0xa000 ? dict->index[cp-0x3400] : 0;
        const char *syllable=index && index<sizeof dict->syllables/sizeof dict->syllables[0] &&
            !dict->syllables[index][7] ? dict->syllables[index] : "";
        if (*syllable) {
            int bytes=strlen(syllable);
            if (length+bytes>=(int)sizeof full || n+1>=(int)sizeof initials) break;
            memcpy(full+length,syllable,bytes);
            length+=bytes;
            initials[n++]=syllable[0];
        } else if ((cp>='A' && cp<='Z') || (cp>='a' && cp<='z') || (cp>='0' && cp<='9')) {
            if (length+1>=(int)sizeof full || n+1>=(int)sizeof initials) break;
            full[length++]=fold_ascii(cp);
            initials[n++]=fold_ascii(cp);
        }
    }
    full[length]=0; initials[n]=0;
    return name_matches(full,spelling) || name_matches(initials,spelling);
}

static int app_matches(const LauncherApp *app,const char *query,const char *spelling) {
    if (!*query || matches_title(app->name,query,spelling)) return 1;
    const AppInfo *info=app->info;
    if (!info) return 0;
    if (matches_title(info->title,query,spelling) || (info->command && name_matches(info->command,query))) return 1;
    static const char *const dock_names[]={"files","browser","calculator","terminal","nano","editor"};
    if (app->dock_id>=0) {
        static const char *const aliases[]={"文件管理器","网页浏览器","计算器","命令行终端","文本编辑","图形文本编辑器"};
        return name_matches(dock_names[app->dock_id],query) || matches_title(aliases[app->dock_id],query,spelling);
    }
    static const struct {const char *file,*alias;} aliases[]={
        {"writer.win3","文字处理 文档编辑器 写作"}, {"ristretto.win3","图片查看器"},
        {"nautilus.win3","文件管理器"}, {"mousepad.win3","文本编辑器"}, {"code","代码编辑器"},
        {"dsh","深度求索 助手"}, {"dsh-gui.win3","深度求索 图形助手"},
        {"ra2-recomp","原版 chongbianyi cby"}, {"simulate.win3","物理仿真"},
        {"freecad","三维建模"}, {"agent.win3","智能助手"}
    };
    for (unsigned i=0;i<sizeof aliases/sizeof aliases[0];i++)
        if (!strcmp(app->name,aliases[i].file)) return matches_title(aliases[i].alias,query,spelling);
    return 0;
}
