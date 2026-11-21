/* Navigation snapshots keep names so changes to a directory between visits
 * cannot silently select a different file at the same row number. */
typedef struct {
    FilePath filter,top,focus,anchor;
    int first,column,descending,count;
    char *names[];
} FileView;
typedef struct { FilePath path; FileView *view; } FileVisit;

static int history_name_order(const void *a,const void *b) {
    return strcmp(*(const char *const *)a,*(const char *const *)b);
}
static FileView *file_view(const char *filter,const Entry *rows,int count,int chosen,
                           int first,int focus,int anchor,int column,int descending) {
    size_t bytes=sizeof(FileView)+chosen*sizeof(char *);
    for (int i=0;i<count;++i) if (rows[i].chosen) bytes+=strlen(rows[i].name)+1;
    FileView *v=malloc(bytes);
    if (!v) return NULL;
    v->first=first; v->column=column; v->descending=descending; v->count=0;
    strcpy(v->filter,filter);
    strcpy(v->focus,focus>=0?rows[focus].name:"");
    strcpy(v->anchor,anchor>=0?rows[anchor].name:"");
    strcpy(v->top,first<count?rows[first].name:"");
    char *next=(char *)(v->names+chosen);
    for (int i=0;i<count;++i) if (rows[i].chosen) {
        v->names[v->count++]=next; strcpy(next,rows[i].name); next+=strlen(next)+1;
    }
    if (chosen>1) qsort(v->names,chosen,sizeof(char *),history_name_order);
    return v;
}
static int view_chosen(FileView *v,const char *name) {
    int lo=0,hi=v->count;
    while (lo<hi) {
        int mid=(lo+hi)/2,c=strcmp(name,v->names[mid]);
        if (!c) return 1;
        if (c<0) hi=mid; else lo=mid+1;
    }
    return 0;
}
