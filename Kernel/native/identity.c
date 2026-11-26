#include "identity.h"
typedef unsigned long word;
extern int memcmp(const void *,const void *,word);
extern void *memcpy(void *,const void *,word);
struct cred { unsigned uid[3],gid[3],mask_plus_one,count,groups[LX_GROUPS]; };
static struct cred credentials[16];
void id_inherit(unsigned child,unsigned parent) {
    if (child<16 && parent<16) credentials[child]=credentials[parent];
}
unsigned id_uid(unsigned slot) { return credentials[slot].uid[1]; }
unsigned id_gid(unsigned slot) { return credentials[slot].gid[1]; }
unsigned id_mask(unsigned slot) {
    return credentials[slot].mask_plus_one ? credentials[slot].mask_plus_one-1 : 022;
}
static int member(const struct cred *c,unsigned gid) {
    if (c->gid[1]==gid) return 1;
    for (unsigned i=0;i<c->count;++i) if (c->groups[i]==gid) return 1;
    return 0;
}
long id_call(unsigned slot,unsigned op,unsigned *v) {
    if (slot>=16) return -22;
    struct cred *c=credentials+slot;
    if (!op) return 1;
    if (op==1) {
        unsigned ids[]={c->uid[0],c->uid[1],c->uid[2],c->gid[0],c->gid[1],c->gid[2]};
        memcpy(v,ids,sizeof(ids)); return 0;
    }
    if (op==4) {
        if (c->uid[1]) return -1;
        if (v[0]>LX_GROUPS) return -22;
        c->count=v[0]; memcpy(c->groups,v+1,c->count*4); return 0;
    }
    if (op==5) { memcpy(v,c->groups,c->count*4); return c->count; }
    if (op==6) { unsigned old=id_mask(slot); c->mask_plus_one=(v[0]&0777)+1;return old; }
    if (op!=2 && op!=3 && (op<7 || op>10)) return -38;
    unsigned *ids=(op==2||op==7||op==9)? c->uid : c->gid;
    unsigned r=v[0],e=v[1],s=v[2],none=~0U;
    if (op==7 || op==8) {
        if (r==none) return -22;
        e=r;
        if (c->uid[1]) { if (r!=ids[0] && r!=ids[2]) return -1; r=s=none; }
        else s=r;
    } else if (op==9 || op==10) {
        if (c->uid[1] && r!=none && r!=ids[0] && r!=ids[1]) return -1;
        s=(r!=none || (e!=none && e!=ids[0])) ? (e==none?ids[1]:e) : none;
    }
    unsigned next[]={r,e,s};
    if (c->uid[1]) {
        for (unsigned i=0;i<3;++i)
            if (next[i]!=none && next[i]!=ids[0] && next[i]!=ids[1] && next[i]!=ids[2]) return -1;
    }
    for (unsigned i=0;i<3;++i) if (next[i]!=none) ids[i]=next[i];
    return 0;
}
int id_access(unsigned slot,const struct lx_mode *f,unsigned bits) {
    const struct cred *c=credentials+slot;
    if (!bits || !c->uid[1]) return 1;
    unsigned shift=c->uid[1]==f->uid ? 6 : member(c,f->gid) ? 3 : 0;
    return ((f->mode>>shift)&bits)==bits;
}
static int under(const char *path,unsigned n,const char *root,unsigned len) {
    return n>=len && !memcmp(path,root,len) && (n==len || path[len]=='/');
}
void id_default(const char *path,unsigned n,int dir,struct lx_mode *out) {
    struct lx_mode m={0,0,dir?0755:0644};
    if (under(path,n,"/bin",4) || under(path,n,"/libexec",8)) m.mode=0755;
    if (under(path,n,"/home/lixin",11)) { m.uid=m.gid=1000; if (n==11) m.mode=0700; }
    if (n==4 && !memcmp(path,"/tmp",4)) m.mode=01777;
    if (under(path,n,"/root",5) || under(path,n,"/etc/shadow",11) ||
        under(path,n,LX_PERMISSIONS,sizeof(LX_PERMISSIONS)-1)) m.mode=dir?0700:0600;
    *out=m;
}
static unsigned field(const unsigned char *p) {
    return p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);
}
long id_find(const unsigned char *data,unsigned size,const char *path,unsigned length,
             struct lx_mode *out,unsigned *offset) {
    if (size>LX_PERM_LIMIT) return -5;
    unsigned pos=0;
    while (pos<size) {
        if (size-pos<16) return -5;
        unsigned n=field(data+pos+12);
        if (!n || n>512 || n>size-pos-16) return -5;
        if (n==length && !memcmp(data+pos+16,path,n)) {
            out->uid=field(data+pos);out->gid=field(data+pos+4);out->mode=field(data+pos+8);
            if (offset) *offset=pos;
            return 1;
        }
        pos+=16+n;
    }
    if (offset) *offset=pos;
    return 0;
}
int id_mode(const unsigned char *data,unsigned size,const char *path,unsigned n,int dir,struct lx_mode *m) {
    id_default(path,n,dir,m);
    return id_find(data,size,path,n,m,0)>=0;
}
static void put32(unsigned char *p,unsigned value) {
    for (unsigned i=0;i<4;++i) p[i]=value>>(8*i);
}
long id_encode(const unsigned char *data,unsigned size,const char *path,unsigned n,const struct lx_mode *mode,
               unsigned char *out,unsigned cap) {
    struct lx_mode old;
    unsigned offset=0;
    long match=id_find(data,size,path,n,&old,&offset);
    if (match<0 || !n || n>512) return -5;
    unsigned skip=match?16+n:0,keep=size-skip,newsize=keep+(mode?16+n:0);
    if (newsize>cap || newsize>LX_PERM_LIMIT) return -28;
    if (offset) memcpy(out,data,offset);
    if (size>offset+skip) memcpy(out+offset,data+offset+skip,size-offset-skip);
    if (mode) {
        put32(out+keep,mode->uid);put32(out+keep+4,mode->gid);
        put32(out+keep+8,mode->mode);put32(out+keep+12,n);memcpy(out+keep+16,path,n);
    }
    return newsize;
}
/* Directory search for each ancestor. 8 requests directory-entry removal,
 * subject to parent write/search and the sticky-directory ownership rule. */
int id_path(unsigned slot,const unsigned char *data,unsigned size,const char *path,unsigned n,int dir,unsigned bits) {
    if (!n || *path!='/' || slot>=16) return 0;
    if (!id_uid(slot)) return 1;
    struct lx_mode parent,m;
    if (!id_mode(data,size,"/",1,1,&parent) || !id_access(slot,&parent,1)) return 0;
    for (unsigned i=1;i<n;++i) {
        if (path[i]!='/') continue;
        if (!id_mode(data,size,path,i,1,&parent) || !id_access(slot,&parent,1)) return 0;
    }
    if (dir<0) return id_access(slot,&parent,3);
    if (!id_mode(data,size,path,n,dir,&m)) return 0;
    if (bits==8) return id_access(slot,&parent,3) &&
        (!(parent.mode&01000) || parent.uid==id_uid(slot) || m.uid==id_uid(slot));
    return id_access(slot,&m,bits);
}
int id_modify(unsigned slot,struct lx_mode *m,unsigned op,unsigned a,unsigned b) {
    unsigned uid=id_uid(slot);
    if (uid && uid!=m->uid) return -1;
    if (op==13) {
        /* set-ID executables require a secure-exec ABI. */
        if (a&06000) return -95;
        m->mode=a&01777;return 0;
    }
    if (op!=14) return -22;
    if (uid && ((a!=~0U && a!=uid) || (b!=~0U && b!=m->gid && !member(credentials+slot,b)))) return -1;
    if (a!=~0U) m->uid=a;
    if (b!=~0U) m->gid=b;
    return 0;
}
