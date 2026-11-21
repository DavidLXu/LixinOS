// HTML foreign-content subtrees use the same static SVG decoder as <img>.
#include <nsutils/base64.h>
struct svg_xml { char *data; size_t len,cap; };
static bool svg_put(struct svg_xml *xml,const char *s,size_t n) {
    if (n>1024*1024-xml->len) return false;
    if (n+xml->len>xml->cap) {
        size_t cap=(n+xml->len)*2+256;
        void *data=realloc(xml->data,cap);
        if (!data) return false;
        xml->data=data; xml->cap=cap;
    }
    memcpy(xml->data+xml->len,s,n); xml->len+=n; return true;
}
static bool svg_string(struct svg_xml *xml,dom_string *s,bool escape) {
    const char *start=dom_string_data(s), *end=start+dom_string_byte_length(s);
    if (escape) for (const char *p=start;p<end;p++) {
        const char *entity= *p=='&' ? "&amp;" : *p=='<' ? "&lt;" :
                            *p=='"' ? "&quot;" : NULL;
        if (!entity) continue;
        if (!svg_put(xml,start,p-start) || !svg_put(xml,entity,strlen(entity))) return false;
        start=p+1;
    }
    return svg_put(xml,start,end-start);
}
static bool svg_element(struct svg_xml *xml,dom_node *node,unsigned depth,const char *root_color) {
    if (depth>=128) return false;
    dom_node_type type;
    if (dom_node_get_node_type(node,&type)!=DOM_NO_ERR) return false;
    if (type==DOM_TEXT_NODE || type==DOM_CDATA_SECTION_NODE) {
        dom_string *value=NULL;
        if (dom_node_get_node_value(node,&value)!=DOM_NO_ERR) return false;
        bool ok=!value || svg_string(xml,value,true);
        if (value) dom_string_unref(value);
        return ok;
    }
    if (type!=DOM_ELEMENT_NODE) return true;
    dom_string *name=NULL;
    if (dom_node_get_node_name(node,&name)!=DOM_NO_ERR || !name) return false;
    bool ok=svg_put(xml,"<",1) && svg_string(xml,name,false);
    dom_namednodemap *attrs=NULL;
    uint32_t n=0;
    if (dom_node_get_attributes(node,&attrs)!=DOM_NO_ERR) ok=false;
    if (attrs && dom_namednodemap_get_length(attrs,&n)!=DOM_NO_ERR) ok=false;
    bool has_color=false;
    for (uint32_t i=0;ok && i<n;i++) {
        dom_node *attr=NULL; dom_string *key=NULL,*value=NULL;
        ok=dom_namednodemap_item(attrs,i,&attr)==DOM_NO_ERR && attr &&
           dom_node_get_node_name(attr,&key)==DOM_NO_ERR && key &&
           dom_node_get_node_value(attr,&value)==DOM_NO_ERR && value;
        if (ok) {
            if (!strcmp(dom_string_data(key),"color")) has_color=true;
            ok=svg_put(xml," ",1) && svg_string(xml,key,false) && svg_put(xml,"=\"",2) &&
               svg_string(xml,value,true) && svg_put(xml,"\"",1);
        }
        if (value) dom_string_unref(value);
        if (key) dom_string_unref(key);
        if (attr) dom_node_unref(attr);
    }
    if (attrs) dom_namednodemap_unref(attrs);
    if (ok && root_color && !has_color) ok=svg_put(xml,root_color,strlen(root_color));
    ok=ok && svg_put(xml,">",1);
    dom_node *child=NULL;
    if (ok && dom_node_get_first_child(node,&child)!=DOM_NO_ERR) ok=false;
    while (child) {
        dom_node *next=NULL;
        ok=ok && svg_element(xml,child,depth+1,NULL) &&
            dom_node_get_next_sibling(child,&next)==DOM_NO_ERR;
        dom_node_unref(child); child=next;
    }
    ok=ok && svg_put(xml,"</",2) && svg_string(xml,name,false) && svg_put(xml,">",1);
    dom_string_unref(name); return ok;
}
static bool inline_svg(dom_node *node,html_content *html,struct box *box,bool *children) {
    *children=false;
    if (!nsoption_bool(foreground_images)) return true;
    struct svg_xml xml={0};
    css_color color=0; css_computed_color(box->style,&color);
    char root_color[24]; snprintf(root_color,sizeof(root_color)," color=\"#%06x\"",color&0xffffff);
    bool ok=svg_element(&xml,node,0,root_color);
    uint8_t *b64=NULL; size_t len=0;
    if (ok) ok=nsu_base64_encode_alloc((uint8_t *)xml.data,xml.len,&b64,&len)==NSUERROR_OK;
    free(xml.data);
    if (!ok) return true;
    static const char prefix[]="data:image/svg+xml;base64,";
    char *address=malloc(sizeof(prefix)+len);
    if (!address) { free(b64); return false; }
    memcpy(address,prefix,sizeof(prefix)-1);
    memcpy(address+sizeof(prefix)-1,b64,len); address[sizeof(prefix)-1+len]=0;
    free(b64);
    nsurl *url=NULL; nserror err=nsurl_create(address,&url); free(address);
    if (err!=NSERROR_OK) return err!=NSERROR_NOMEM;
    box->flags|=IS_REPLACED;
    ok=html_fetch_object(html,url,box,image_types,false);
    nsurl_unref(url);
    return ok;
}
