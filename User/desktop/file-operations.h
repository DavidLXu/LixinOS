/* Included by both desktop builds. Operations keep the guest transaction open
 * only while applying an entire selection, never while waiting for a dialog. */
static void clear_batch(FileBatch *b) {
    free(b->items);
    *b=(FileBatch){0};
}
static int read_path_record(const char *path,char out[513]) {
    long n=lx_stat(path);
    if (n<1 || n>512) return 0;
    int fd=lx_open(path), used=0;
    if (fd<0) return 0;
    while (used<n) {
        int got=lx_read(fd,out+used,n-used);
        if (got<=0) break;
        used+=got;
    }
    lx_close(fd);
    out[used]=0;
    char canonical[513];
    return used==n && strlen(out)==(size_t)n && out[0]=='/' &&
        ui_file_path(canonical,"/",out) && !strcmp(canonical,out);
}
static int file_error(Desktop *d,const char *message) {
    strcpy(d->message,message);
    return 0;
}
static int at_trash_root(const char *path) {
    return !strncmp(path,"/Trash/",7) && !strchr(path+7,'/');
}
#define TRASH_RECORDS "/Trash/.origins"
static int reserved_path(const char *path) {
    size_t n=sizeof(TRASH_RECORDS)-1;
    return !strncmp(path,TRASH_RECORDS,n) && (!path[n] || path[n]=='/');
}
static void origin_record(const char *path,char out[513]) {
    /* A fixed-length key also accommodates the longest guest filenames. */
    unsigned long hash=14695981039346656037UL;
    for (const unsigned char *s=(const unsigned char *)path;*s;++s) hash=(hash^*s)*1099511628211UL;
    strcpy(out,TRASH_RECORDS "/"); number(out+strlen(out),hash);
}
static int move_with_record(Desktop *d,FilePair *p) {
    char record[513];
    if (at_trash_root(p->source)) {
        origin_record(p->source,record);
        long exists=fs_request(d,FS_EXISTS,record,NULL);
        if (exists<0) return 0;
        if (exists) {
            if (!read_path_record(record,p->origin)) return file_error(d,"原位置记录无效");
            if (fs_request(d,FS_UNLINK,record,NULL)<0) return 0;
        }
    }
    if (at_trash_root(p->target)) {
        if (fs_request(d,FS_MKDIR|256,TRASH_RECORDS,NULL)<0) return 0;
        origin_record(p->target,record);
        if (fs_request(d,FS_PATH_RECORD,record,*p->origin?p->origin:p->source)<0) return 0;
    }
    return fs_request(d,FS_MOVE,p->source,p->target)>=0;
}
static int capture_selection(Desktop *d,int cut) {
    FilePath *paths=malloc((size_t)d->chosen*sizeof *paths);
    if (!paths) return file_error(d,"内存不足");
    int n=0;
    for (int i=0;i<d->count;++i) if (d->rows[i].chosen) {
        if (!child_path(d,paths[n++],d->path,d->rows[i].name)) { free(paths); return 0; }
    }
    free(d->clipboard); d->clipboard=paths; d->clipboard_count=n; d->cut=cut;
    strcpy(d->message,"请在目标目录粘贴");
    return 1;
}
static int prepare_files(Desktop *d,int action,const char *name) {
    FileBatch *b=&d->pending;
    clear_batch(b);
    int count=action==F_UNDO ? d->undo.count : action==F_PASTE ? d->clipboard_count :
        action==F_NEW_DIR || action==F_NEW_FILE ? 1 : d->chosen;
    if (!count) return 0;
    b->items=malloc((size_t)count*sizeof *b->items);
    if (!b->items) return file_error(d,"内存不足");
    memset(b->items,0,(size_t)count*sizeof *b->items);
    b->count=count; b->action=action; b->conflict=-1;
    if (action==F_UNDO) { memcpy(b->items,d->undo.items,(size_t)count*sizeof *b->items); return 1; }
    int row=0;
    for (int i=0;i<count;++i) {
        FilePair *p=&b->items[i];
        if (action==F_NEW_DIR || action==F_NEW_FILE) return child_path(d,p->target,d->path,name);
        if (action==F_PASTE) strcpy(p->source,d->clipboard[i]);
        else {
            while (row<d->count && !d->rows[row].chosen) ++row;
            if (row==d->count || !child_path(d,p->source,d->path,d->rows[row++].name)) return 0;
        }
        if (action==F_RESTORE) {
            char record[513]; origin_record(p->source,record);
            if (!read_path_record(record,p->target))
                return file_error(d,"没有原位置记录，可用剪切和粘贴恢复");
        } else if (!child_path(d,p->target,action==F_TRASH?"/Trash":d->path,
                              action==F_RENAME?name:strrchr(p->source,'/')+1)) return 0;
    }
    return 1;
}
static int apply_item(Desktop *d,FileBatch *b,int index) {
    FilePair *p=&b->items[index];
    int a=b->action;
    if (reserved_path(p->source) || reserved_path(p->target)) return file_error(d,"此位置用于废纸篓记录");
    if (a==F_TRASH && (!strcmp(p->source,"/Trash") || at_trash_root(p->source)))
        return file_error(d,"此项目已在废纸篓");
    if (a==F_TRASH && fs_request(d,FS_MKDIR|256,"/Trash",NULL)<0) return 0;
    long occupied=fs_request(d,FS_EXISTS,p->target,NULL);
    if (a==F_TRASH && occupied>0) {
        char base[513]; strcpy(base,p->target);
        for (unsigned suffix=2;occupied>0;++suffix) {
            char digits[32]; number(digits,suffix);
            int n=strlen(base);
            while (n+1+strlen(digits)>512) n=ui_prev(base,n);
            memcpy(p->target,base,n); p->target[n]='~'; strcpy(p->target+n+1,digits);
            occupied=fs_request(d,FS_EXISTS,p->target,NULL);
        }
    }
    if (occupied<0) return 0;
    if (occupied) {
        if (a==F_RESTORE || a==F_UNDO) {
            b->conflict=index;
            strcpy(d->filename,strrchr(p->target,'/')+1); name_begin(d,1);
            d->naming=FILE_PROMPT+a;
        }
        return file_error(d,"目标已存在，请另选名称");
    }
    if (a==F_NEW_DIR || a==F_NEW_FILE)
        return fs_request(d,a==F_NEW_DIR?FS_MKDIR:FS_TOUCH,p->target,NULL)>=0;
    size_t n=strlen(p->source);
    if (!strncmp(p->source,p->target,n) && (p->target[n]=='/' || !p->target[n]))
        return file_error(d,"目标与来源重合");
    if (a==F_PASTE && !d->cut) {
        if (lx_stat(p->source)!=LX_DIR) return fs_request(d,FS_COPY,p->source,p->target)>=0;
        FilePath from,to; strcpy(from,p->source); strcpy(to,p->target);
        return copy_tree(d,from,to);
    }
    return move_with_record(d,p);
}
static int commit_files(Desktop *d) {
    FileBatch *b=&d->pending;
    int a=b->action;
    if (fs_request(d,FS_BEGIN,NULL,NULL)<0) return 0;
    for (int i=0;i<b->count;++i) if (!apply_item(d,b,i)) {
        fs_request(d,FS_ABORT,NULL,NULL); return 0;
    }
    if (fs_request(d,FS_COMMIT,NULL,NULL)<0) return 0;
    if (a==F_UNDO) clear_batch(&d->undo);
    if (a==F_RENAME || a==F_TRASH || a==F_RESTORE || (a==F_PASTE && d->cut)) {
        for (int i=0;i<b->count;++i) {
            FilePath swap;
            strcpy(swap,b->items[i].source); strcpy(b->items[i].source,b->items[i].target);
            strcpy(b->items[i].target,swap);
        }
        clear_batch(&d->undo);
        d->undo=*b; *b=(FileBatch){0};
    }
    if (a==F_PASTE && d->cut) { free(d->clipboard); d->clipboard=NULL; d->clipboard_count=0; }
    clear_batch(b); d->naming=0; d->first=0; load(d);
    strcpy(d->message,a==F_TRASH?"已移入废纸篓":a==F_UNDO?"已撤销":"操作完成");
    return 1;
}
static int apply_file_action(Desktop *d,int a,const char *name) {
    return prepare_files(d,a,name) && commit_files(d);
}
static int retry_destination(Desktop *d) {
    FileBatch *b=&d->pending;
    if (b->conflict<0 || b->conflict>=b->count) return 0;
    FilePair *p=&b->items[b->conflict];
    FilePath parent; strcpy(parent,p->target);
    char *slash=strrchr(parent,'/'); slash[slash==parent]=0;
    if (!child_path(d,p->target,parent,d->filename)) return 0;
    return commit_files(d);
}

#define FILE_TYPES "/home/lixin/.file-types"
static int association_path(Desktop *d,char path[513]) {
    const char *ext=extension(d->rows[d->marked].name);
    if (!ext || strlen(ext)+sizeof(FILE_TYPES)>512) return 0;
    strcpy(path,FILE_TYPES "/");
    char *out=path+strlen(path);
    while (*ext) *out++=fold_ascii(*ext++);
    *out=0;
    return 1;
}
static int preferred_editor(Desktop *d) {
    FilePath path,value;
    return association_path(d,path) && read_path_record(path,value) && !strcmp(value,"/bin/text-editor");
}
static int set_editor(Desktop *d,int graphical) {
    FilePath path;
    if (!association_path(d,path)) return file_error(d,"扩展名太长或不存在");
    if (fs_request(d,FS_BEGIN,NULL,NULL)<0) return 0;
    const char *dirs[]={"/home","/home/lixin",FILE_TYPES};
    for (int i=0;i<3;++i) if (fs_request(d,FS_MKDIR|256,dirs[i],NULL)<0) return 0;
    long exists=fs_request(d,FS_EXISTS,path,NULL);
    if (exists<0 || (exists && fs_request(d,FS_UNLINK,path,NULL)<0) ||
        fs_request(d,FS_PATH_RECORD,path,graphical?"/bin/text-editor":"/bin/nano")<0) return 0;
    return fs_request(d,FS_COMMIT,NULL,NULL)>=0;
}
