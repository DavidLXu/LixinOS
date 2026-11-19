#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "load.h"
#include "tls.h"
#include "cookies.h"
#include "../tcc/lixin.h"


#define HEADER_LIMIT (16 * 1024)
extern unsigned char *join_urls(unsigned char *, unsigned char *);
extern void mem_free(void *);

static const char *local_page(const char *url, struct page_source *s) {
    char path[513];
    size_t n = 0;
    if (*url != '/') return "Expected an absolute file URL";
    while (*url && *url != '#' && *url != '?') {
        int c = (unsigned char)*url++;
        if (c == '%') {
            if (!isxdigit((unsigned char)url[0]) || !isxdigit((unsigned char)url[1]))
                return "Invalid file URL escape";
            char hex[] = {url[0],url[1],0};
            c = strtol(hex,NULL,16); url += 2;
        }
        if (c < 32 || n == sizeof(path)-1) return "Invalid file path";
        path[n++] = c;
    }
    path[n] = 0;
    long size = lx_stat(path);
    if (size < 0) return "File not found or not a regular file";
    if (size > PAGE_LIMIT) return "Page exceeds 10 MiB";
    int fd = lx_open(path);
    if (fd < 0) return "Cannot open file";
    s->bytes = malloc(size+1);
    size_t used = 0;
    while (s->bytes && used < (size_t)size) {
        int got = lx_read(fd,s->bytes+used,size-used);
        if (got <= 0) break;
        used += got;
    }
    lx_close(fd);
    if (!s->bytes || used != (size_t)size) return "Could not read file";
    s->body=s->bytes; s->length=used; s->body[used]=0;
    return NULL;
}

static const char *wait_io(int *idle) {
    if (++*idle > 1500) return "HTTP server timed out";
    if (lx_timed_byte(8)==3) return "Cancelled";
    return NULL;
}
/* Check message framing as records arrive. An authenticated final chunk or
   Content-Length completes the response without waiting for transport EOF. */
struct framing {
    size_t header, total, next_chunk, decoded;
    int chunked, trailers;
};
static int complete(struct framing *f,struct page_source *s) {
    char *buf=s->bytes;
    if (!f->header) {
        char *end=strstr(buf,"\r\n\r\n");
        if (!end) return s->length>=HEADER_LIMIT?-1:0;
        f->header=end+4-buf;
        if (f->header>HEADER_LIMIT) return -1;
        char *line=strstr(buf,"\r\n");
        if (!line) return -1;
        for (line+=2;line<end;) {
            char *next=strstr(line,"\r\n");
            if (!next) return -1;
            if (!strncasecmp(line,"Content-Length:",15)) {
                char *v=line+15,*tail;
                while (*v==' '||*v=='\t') ++v;
                unsigned long n=strtoul(v,&tail,10);
                if (!isdigit((unsigned char)*v) || tail!=next || f->total) return -1;
                if (n>PAGE_LIMIT) return -2;
                f->total=f->header+n;
            }
            if (!strncasecmp(line,"Transfer-Encoding:",18)) {
                char *v=line+18;
                while (*v==' '||*v=='\t') ++v;
                if (next-v!=7 || strncasecmp(v,"chunked",7) || f->chunked) return -1;
                f->chunked=1;
            }
            line=next+2;
        }
        if (f->chunked && f->total) return -1;
        f->next_chunk=f->header;
    }
    if (f->total) return s->length>=f->total;
    if (!f->chunked) return 0;
    while (f->next_chunk<s->length) {
        char *line=buf+f->next_chunk, *end=strstr(line,"\r\n");
        if (!end) return s->length-f->next_chunk>HEADER_LIMIT?-1:0;
        if (f->trailers) {
            f->next_chunk=end+2-buf;
            if (end==line) {
                s->length=f->header+f->decoded;
                buf[s->length]=0;
                return 1;
            }
            continue;
        }
        char *tail;
        unsigned long n=strtoul(line,&tail,16);
        if (!isxdigit((unsigned char)*line) || (tail!=end && *tail!=';')) return -1;
        if (n>PAGE_LIMIT-f->decoded) return -2;
        size_t start=end+2-buf;
        if (!n) {f->trailers=1;f->next_chunk=start;continue;}
        if (s->length-start<n+2) return 0;
        if (memcmp(buf+start+n,"\r\n",2)) return -1;
        memmove(buf+f->header+f->decoded,buf+start,n);
        f->decoded+=n;
        f->next_chunk=start+n+2;
    }
    return 0;
}
static const char *exchange(int fd, struct tls_connection *tls, const char *request, struct page_source *s) {
    int idle=0;
    size_t left=strlen(request);
    if (tls) {
        const char *error=tls_send(tls,request,left);
        if (error) return error;
        left=0;
    }
    while (left) {
        int n=lx_tcp_write(fd,request,left);
        if (n==LX_TCP_AGAIN) {
            const char *error=wait_io(&idle);
            if (error) return error;
        } else if (n<=0) return "HTTP request failed";
        else { request+=n; left-=n; idle=0; }
    }
    struct framing framing={0};
    const size_t limit=PAGE_LIMIT+HEADER_LIMIT+1024*1024;
    size_t capacity=16384;
    s->bytes=malloc(capacity+1);
    if (!s->bytes) return "Not enough memory for HTTP response";
    for (;;) {
        if (s->length==capacity) {
            capacity=capacity*2>limit+1 ? limit+1 : capacity*2;
            char *grown=realloc(s->bytes,capacity+1);
            if (!grown) return "Not enough memory for HTTP response";
            s->bytes=grown;
        }
        int n=tls ? tls_receive(tls,s->bytes+s->length,capacity-s->length) : lx_tcp_read(fd,s->bytes+s->length,capacity-s->length);
        if (!n) {if (framing.chunked || (framing.total && s->length<framing.total)) return "Incomplete HTTP response";break;}
        if (n==LX_TCP_AGAIN) {
            const char *error=wait_io(&idle);
            if (error) return error;
        } else if (n<0) return tls ? tls_error(tls) : "HTTP read failed";
        else {
            idle=0; s->length+=n;
            if (s->length>limit) return "HTTP response is too large";
            s->bytes[s->length]=0;
            int done=complete(&framing,s);
            if (done==-2) return "Page exceeds 10 MiB";
            if (done<0) return "Invalid HTTP framing";
            if (done) {if (framing.total) s->length=framing.total;break;}
        }
    }
    s->bytes[s->length]=0;
    return NULL;
}
static const char *http_page(char *address, struct page_source *s, char *redirect,int credentials) {
    int secure = !strncmp(address,"https://",8);
    const char *host=address+(secure?8:7);
    size_t hn=strcspn(host,":/?#");
    char name[254];
    if (!hn || hn >= sizeof(name)) return "Invalid hostname";
    for (size_t i=0; i<hn; ++i) {
        unsigned char c=host[i];
        if (!isalnum(c) && c!='-' && c!='.') return "Invalid hostname";
    }
    memcpy(name,host,hn); name[hn]=0;
    const char *path=host+hn;
    long port=secure?443:80;
    if (*path==':') {
        char *end;
        port=strtol(++path,&end,10);
        if (!isdigit((unsigned char)*path) || port<=0 || port>65535) return "Invalid port";
        path=end;
    }
    if (*path && !strchr("/?#",*path)) return "Invalid HTTP URL";
    char cookies[4096],request[ADDRESS_LIMIT+4600];
    cookies[0]=0;
    if (credentials) cookie_list(address,cookies,sizeof(cookies),1);
    int path_len=strcspn(path,"#");
    for (int i=0;i<path_len;i++) if ((unsigned char)path[i]<=32 || path[i]==127) return "Invalid HTTP path";
    snprintf(request,sizeof(request),"GET %s%.*s HTTP/1.1\r\nHost: %s:%ld\r\nConnection: close\r\nAccept-Encoding: identity\r\nUser-Agent: Mozilla/5.0 (compatible; Links/2.30; LixinOS aarch64)\r\n%s%s%s\r\n",
             *path=='/'?"":"/",path_len,path,name,port,*cookies?"Cookie: ":"",cookies,*cookies?"\r\n":"");
    int fd=lx_tcp_connect(name,port);
    if (fd<0) return "DNS lookup or TCP connection failed";
    struct tls_connection *tls=secure ? tls_open(fd,name) : NULL;
    const char *error=(secure && !tls) ? "TLS setup failed" : exchange(fd,tls,request,s);
    tls_free(tls);
    lx_tcp_close(fd);
    if (error) return error;
    char *end=strstr(s->bytes,"\r\n\r\n");
    if (!end || end-s->bytes>HEADER_LIMIT) return "Invalid HTTP headers";
    s->body=end+4; s->headers=s->bytes; end[2]=0;
    s->length-=s->body-s->bytes;
    if (s->length>PAGE_LIMIT) return "Page exceeds 10 MiB";
    int status=0;
    if (sscanf(s->bytes,"HTTP/%*u.%*u %d",&status)!=1) return "Invalid HTTP status";
    s->status=status;
    for (char *p=strstr(s->bytes,"\r\n")+2; p<end; ) {
        char *next=strstr(p,"\r\n");
        if (!next) return "Invalid HTTP header";
        if (credentials && !strncasecmp(p,"Set-Cookie:",11)) {
            char *v=p+11;while (v<next && *v==' ') ++v;
            cookie_store(address,v,next-v,1);
        }
        if (status>=300 && status<400 && !strncasecmp(p,"Location:",9)) {
            char *v=p+9;
            while (v<next && (*v==' '||*v=='\t')) v++;
            if (next-v>=ADDRESS_LIMIT) return "Redirect URL too long";
            memcpy(redirect,v,next-v); redirect[next-v]=0;
        }
        if (!strncasecmp(p,"Content-Encoding:",17) && strncasecmp(p+17," identity\r\n",11))
            return "Unsupported HTTP content encoding";
        if (!strncasecmp(p,"Content-Length:",15)) {
            char *tail;
            unsigned long length=strtoul(p+15,&tail,10);
            if (tail!=next || length!=s->length) return "Incomplete HTTP response";
        }
        p=next+2;
    }
    if (status>=300 && status<400 && *redirect) return NULL;

    return NULL;
}
static size_t origin_length(const char *url) {
    size_t scheme=!strncmp(url,"https://",8)?8:!strncmp(url,"http://",7)?7:0;
    return scheme ? scheme+strcspn(url+scheme,"/?#") : 0;
}
static const char *load(char address[ADDRESS_LIMIT],struct page_source *out,const char *origin,int strict) {
    for (int hop=0;hop<6;hop++) {
        /* The upstream relative-URL parser requires a path, even at a site's root. */
        int scheme=!strncmp(address,"https://",8)?8:!strncmp(address,"http://",7)?7:0;
        if (scheme) {
            char *path=address+scheme+strcspn(address+scheme,"/?#");
            if (*path!='/') {
                if (strlen(address)+1>=ADDRESS_LIMIT) return "Address too long";
                memmove(path+1,path,strlen(path)+1);
                *path='/';
            }
        }
        int credentials=1;
        if (origin) {
            size_t n=origin_length(address);
            credentials=n && n==origin_length(origin) && !strncasecmp(address,origin,n);
            if (!credentials && strict)
                return "Cross-origin fetch is unavailable";
        }
        struct page_source s={0};
        char redirect[ADDRESS_LIMIT]={0};
        const char *error;
        if (!strncmp(address,"file://",7)) error=local_page(address+7,&s);
        else if (!strncmp(address,"http://",7) || !strncmp(address,"https://",8)) error=http_page(address,&s,redirect,credentials);
        else return "Supported addresses: local files, HTTP and HTTPS";
        if (!error && !strict && !redirect[0] && s.status && (s.status<200 || s.status>=300))
            error="HTTP server returned an unsuccessful status";
        if (!error && !redirect[0]) { *out=s; return NULL; }
        free(s.bytes);
        if (error) return error;
        char *next=(char *)join_urls((unsigned char *)address,(unsigned char *)redirect);
        if (!next) return "Invalid redirect";
        if ((strncmp(next,"https://",8) && (strncmp(next,"http://",7) || !strncmp(address,"https://",8))) || strlen(next)>=ADDRESS_LIMIT) error="Unsupported redirect";
        else strcpy(address,next);
        mem_free(next);
        if (error) return error;
    }
    return "Too many HTTP redirects";
}

const char *load_page(char address[ADDRESS_LIMIT],struct page_source *out) {
    return load(address,out,NULL,0);
}
const char *load_resource(char address[ADDRESS_LIMIT],const char *parent,struct page_source *out) {
    return load(address,out,parent,0);
}
const char *fetch_same_origin(const char *base,const char *ref,struct page_source *out) {
    if (!origin_length(base) || strlen(ref)>=ADDRESS_LIMIT) return "fetch requires an HTTP origin";
    char *url=(char *)join_urls((unsigned char *)base,(unsigned char *)ref);
    if (!url) return "Invalid fetch URL";
    char address[ADDRESS_LIMIT];
    int fits=strlen(url)<sizeof(address);
    if (fits) strcpy(address,url);
    mem_free(url);
    return fits?load(address,out,base,1):"Fetch URL too long";
}
