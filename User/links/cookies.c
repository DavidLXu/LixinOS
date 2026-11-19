#define _DEFAULT_SOURCE 1
#define _XOPEN_SOURCE 700
#include "cookies.h"
#include "../console.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

enum { COOKIE_COUNT=64 };
struct cookie {
    char host[254], path[256], name[96], value[2048];
    unsigned long expiry;
    int secure, http_only;
};
static struct cookie jar[COOKIE_COUNT];

static const char *host_path(const char *url,char *host,size_t cap) {
    int scheme=!strncmp(url,"https://",8)?8:!strncmp(url,"http://",7)?7:0;
    if (!scheme) return NULL;
    const char *begin=url+scheme;
    size_t n=strcspn(begin,":/?#");
    if (!n || n>=cap) return NULL;
    memcpy(host,begin,n);host[n]=0;
    const char *path=begin+strcspn(begin,"/?#");
    return *path=='/'?path:"/";
}
static int matches_path(const char *request,const char *path) {
    size_t n=strlen(path);
    return !strncmp(request,path,n) && (path[n-1]=='/' || !request[n] || strchr("/?#",request[n]));
}
size_t cookie_list(const char *url,char *out,size_t capacity,int for_http) {
    char host[254]; const char *path=host_path(url,host,sizeof(host));
    size_t length=0;out[0]=0;
    if (!path) return 0;
    unsigned long now=console_call(36,0);
    for (int i=0;i<COOKIE_COUNT;i++) {
        struct cookie *c=&jar[i];
        if (!c->name[0] || c->expiry<=now || strcasecmp(host,c->host) || !matches_path(path,c->path) ||
            (c->secure && strncmp(url,"https://",8)) || (c->http_only && !for_http)) continue;
        size_t n=strlen(c->name)+1+strlen(c->value)+(length?2:0);
        if (length+n>=capacity) continue;
        if (length) strcat(out,"; ");
        strcat(out,c->name);strcat(out,"=");strcat(out,c->value);length+=n;
    }
    return length;
}
void cookie_store(const char *url,const char *line,size_t length,int from_http) {
    struct cookie c={0};
    const char *path=host_path(url,c.host,sizeof(c.host));
    char tmp[4096];
    if (!path || !length || length>=sizeof(tmp)) return;
    for (size_t i=0;i<length;i++) if ((unsigned char)line[i]<32 || (unsigned char)line[i]>=127) return;
    memcpy(tmp,line,length);tmp[length]=0;
    char *cursor=tmp,*pair=strsep(&cursor,";"),*eq=strchr(pair,'=');
    if (!eq) return;
    *eq++=0;
    if (!*pair || strlen(pair)>=sizeof(c.name) || strlen(eq)>=sizeof(c.value) ||
        strpbrk(pair," ()<>@,;:\\\"/[]?={}")) return;
    strcpy(c.name,pair);strcpy(c.value,eq);
    size_t prefix=strcspn(path,"?#");
    while (prefix && path[prefix-1]!='/') prefix--;
    if (prefix>1) prefix--;
    if (prefix>=sizeof(c.path)) return;
    memcpy(c.path,path,prefix);if (!prefix) strcpy(c.path,"/");
    c.expiry=ULONG_MAX;
    unsigned long now=console_call(36,0);
    int https=!strncmp(url,"https://",8),has_domain=0,root_path=0,has_age=0;
    char *attr;
    while ((attr=strsep(&cursor,";"))) {
        while (*attr==' ') attr++;
        char *value=strchr(attr,'=');
        if (value) *value++=0;
        if (!strcasecmp(attr,"Secure")) c.secure=1;
        else if (!strcasecmp(attr,"HttpOnly")) c.http_only=1;
        else if (value && !strcasecmp(attr,"Domain")) {
            if (*value=='.') value++;
            size_t hn=strlen(c.host),dn=strlen(value);
            if (!dn || dn>hn || strcasecmp(c.host+hn-dn,value) || (dn<hn && c.host[hn-dn-1]!='.')) return;
            has_domain=1;
        } else if (value && !strcasecmp(attr,"Path") && *value=='/') {
            if (strlen(value)>=sizeof(c.path)) return;
            strcpy(c.path,value);root_path=!strcmp(value,"/");
        } else if (value && !strcasecmp(attr,"Max-Age")) {
            char *end;long age=strtol(value,&end,10);
            if (end!=value && !*end) {has_age=1;c.expiry=age<=0?0:now+(unsigned long)age;}
        } else if (value && !has_age && !strcasecmp(attr,"Expires")) {
            /* HTTP dates are UTC. newlib starts with the UTC timezone. */
            struct tm tm={0};
            char buf[128];size_t n=strlen(value);
            if (n>=sizeof(buf)) continue;
            memcpy(buf,value,n+1);for (char *p=buf;*p;p++) if (*p=='-') *p=' ';
            if (strptime(buf,"%a, %d %b %Y %H:%M:%S GMT",&tm)) {
                time_t t=mktime(&tm);c.expiry=t>0?(unsigned long)t:0;
            }
        }
    }
    if ((c.secure && !https) || (c.http_only && !from_http)) return;
    if (!strncmp(c.name,"__Secure-",9) && !c.secure) return;
    if (!strncmp(c.name,"__Host-",7) && (!c.secure || has_domain || !root_path)) return;
    int slot=-1;
    for (int i=0;i<COOKIE_COUNT;i++) {
        struct cookie *old=&jar[i];
        if (!strcmp(c.name,old->name) && !strcasecmp(c.host,old->host) && !strcmp(c.path,old->path)) {
            if ((old->http_only && !from_http) || (old->secure && !https)) return;
            slot=i;break;
        }
        if (slot<0 && (!old->name[0] || old->expiry<=now)) slot=i;
    }
    if (slot>=0) jar[slot]=c;
}
