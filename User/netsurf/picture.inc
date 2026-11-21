/* HTML responsive images. Evaluate viewport conditions in CSS pixels and
 * choose the best supported source for the framebuffer's logical pixel grid. */
#include <ctype.h>
#include <strings.h>
#include <math.h>
#include "content/content_factory.h"
static dom_string *picture_attr(dom_node *node, const char *name) {
    dom_string *key=NULL, *value=NULL;
    if (dom_string_create_interned((const uint8_t *)name,strlen(name),&key)==DOM_NO_ERR) {
        dom_element_get_attribute(node,key,&value); dom_string_unref(key);
    }
    return value;
}
static bool picture_tag(dom_node *node, const char *tag) {
    dom_string *name=NULL;
    if (!node || dom_node_get_node_name(node,&name)!=DOM_NO_ERR || !name) return false;
    bool match=!strcasecmp(dom_string_data(name),tag);
    dom_string_unref(name); return match;
}
static bool picture_type(dom_node *node) {
    dom_string *type=picture_attr(node,"type");
    if (!type) return true;
    lwc_string *mime;
    bool ok=false;
    if (lwc_intern_string(dom_string_data(type),dom_string_byte_length(type),&mime)==lwc_error_ok) {
        ok=content_factory_type_from_mime_type(mime)==CONTENT_IMAGE; lwc_string_unref(mime);
    }
    dom_string_unref(type); return ok;
}
static const char *pic_space(const char *p) {
    while (isspace((unsigned char)*p)) ++p;
    return p;
}
static bool picture_media(const char *s,int width,int height) {
    s=pic_space(s);
    if (!*s) return true;
    while (*s) {
        bool ok=true;
        for (;;) {
            s=pic_space(s);
            if (!strncasecmp(s,"screen",6)) s+=6;
            else if (!strncasecmp(s,"all",3)) s+=3;
            else if (*s=='(') {
                const char *p=pic_space(s+1), *end=strchr(p,')');
                if (!end) return false;
                bool min=!strncasecmp(p,"min-",4), max=!strncasecmp(p,"max-",4);
                if (min || max) p+=4;
                int actual;
                if (!strncasecmp(p,"width",5)) { actual=width; p+=5; }
                else if (!strncasecmp(p,"height",6)) { actual=height; p+=6; }
                else { actual=0; ok=false; }
                p=pic_space(p);
                if (*p!=':') ok=false;
                else {
                    p=pic_space(p+1); char *tail; double n=strtod(p,&tail);
                    if (tail==p || strncasecmp(tail,"px",2) || pic_space(tail+2)!=end) ok=false;
                    else ok &= min ? actual>=n : max ? actual<=n : actual==n;
                }
                s=end+1;
            } else { ok=false; while (*s && *s!=',') ++s; }
            s=pic_space(s);
            if (strncasecmp(s,"and",3)) break;
            s+=3;
        }
        if (ok && (!*s || *s==',')) return true;
        s=strchr(s,','); if (!s) return false; ++s;
    }
    return false;
}
static double picture_slot(dom_node *node,int width,int height) {
    dom_string *sizes=picture_attr(node,"sizes");
    if (!sizes) return width;
    char *list=strdup(dom_string_data(sizes)); dom_string_unref(sizes);
    if (!list) return width;
    double slot=width; char *save=NULL;
    for (char *part=strtok_r(list,",",&save);part;part=strtok_r(NULL,",",&save)) {
        char *p=(char *)pic_space(part), *end=strrchr(p,')');
        if (end) { char after=end[1]; end[1]=0; bool ok=picture_media(p,width,height); end[1]=after;
            if (!ok) continue;
            p=(char *)pic_space(end+1);
        }
        char *unit; double n=strtod(p,&unit);
        if (unit==p || n<=0 || !isfinite(n)) continue;
        if (!strncasecmp(unit,"px",2) && !*pic_space(unit+2)) slot=n;
        else if (!strncasecmp(unit,"vw",2) && !*pic_space(unit+2)) slot=n*width/100.;
        else continue;
        break;
    }
    free(list); return slot;
}
static dom_string *picture_candidate(dom_node *node,int width,int height) {
    dom_string *set=picture_attr(node,"srcset"), *best=NULL;
    if (!set) return NULL;
    double slot=picture_slot(node,width,height), density=0;
    const char *p=dom_string_data(set);
    while (*(p=pic_space(p))) {
        if (*p==',') { ++p; continue; }
        const char *url=p;
        while (*p && !isspace((unsigned char)*p)) ++p;
        size_t len=p-url;
        double d=1;
        if (len && url[len-1]==',') --len;
        else {
            p=pic_space(p);
            if (*p && *p!=',') {
                char *end;
                d=strtod(p,&end);
                if (end==p || (*end!='x' && *end!='w')) d=0;
                else {
                    if (*end=='w') d/=slot;
                    p=pic_space(end+1);
                    if (*p && *p!=',') d=0;
                }
                while (*p && *p!=',') ++p;
            }
        }
        if (len && isfinite(d) && d>0 && (!best || (density<1 && d>density) ||
                      (d>=1 && d<density))) {
            dom_string *candidate=NULL;
            if (dom_string_create((const uint8_t *)url,len,&candidate)==DOM_NO_ERR) {
                if (best) dom_string_unref(best);
                best=candidate; density=d;
            }
        }
    }
    dom_string_unref(set); return best;
}
static dom_string *picture_url(dom_node *img,html_content *html) {
    int w=FIXTOINT(html->media.width), h=FIXTOINT(html->media.height);
    if (w<=0) w=800;
    if (h<=0) h=600;
    dom_node *parent=NULL, *node=NULL;
    dom_string *best=NULL, *placeholder=NULL;
    dom_node_get_parent_node(img,&parent);
    if (picture_tag(parent,"picture")) dom_node_get_first_child(parent,&node);
    while (node && node!=img && !best) {
        if (picture_tag(node,"source") && picture_type(node)) {
            dom_string *media=picture_attr(node,"media");
            if (!media || picture_media(dom_string_data(media),w,h))
                best=picture_candidate(node,w,h);
            /* Some lazy loaders mark an inline GIF as empty and keep the real
             * image in img.src. Keep the GIF only if no usable source exists. */
            if (best && !strncasecmp(dom_string_data(best),"data:image/gif;",15)) {
                dom_string *empty=picture_attr(node,"data-empty");
                if (empty) {
                    dom_string_unref(empty);
                    if (placeholder) dom_string_unref(best);
                    else placeholder=best;
                    best=NULL;
                }
            }
            if (media) dom_string_unref(media);
        }
        dom_node *next=NULL;
        dom_node_get_next_sibling(node,&next); dom_node_unref(node); node=next;
    }
    if (node) dom_node_unref(node);
    if (parent) dom_node_unref(parent);
    if (!best) best=picture_candidate(img,w,h);
    if (!best) best=picture_attr(img,"src");
    if (placeholder && (!best || !*pic_space(dom_string_data(best)))) {
        if (best) dom_string_unref(best);
        return placeholder;
    }
    if (placeholder) dom_string_unref(placeholder);
    return best;
}
