#pragma once
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Small arithmetic grammar. Exponentiation is right-associative and binds
 * tighter than unary minus. The input editor limits formulas to 95 bytes. */
typedef struct { const char *s; double t; int error; } Formula;
static void spaces(Formula *f) { while (isspace((unsigned char)*f->s)) ++f->s; }
static int take(Formula *f,char ch) {
    spaces(f); if (*f->s!=ch) return 0;
    ++f->s; return 1;
}
static double sum_expr(Formula *f);
static double factor(Formula *f);
static double atom(Formula *f) {
    if (take(f,'(')) {
        double v=sum_expr(f); if (!take(f,')')) f->error=1; return v;
    }
    if (isalpha((unsigned char)*f->s)) {
        const char *name=f->s;
        while (isalpha((unsigned char)*f->s)) ++f->s;
        size_t len=f->s-name;
        if (len==1 && (*name=='t'||*name=='x')) return f->t;
        if (len==2 && !memcmp(name,"pi",2)) return 3.141592653589793;
        if (!take(f,'(')) { f->error=1; return 0; }
        double a=sum_expr(f);
        if (!take(f,')')) f->error=1;
        static const struct { const char *name; double (*fn)(double); } funcs[]={
            {"sin",sin},{"cos",cos},{"tan",tan},{"sqrt",sqrt},{"exp",exp},{"log",log},{"abs",fabs}};
        for (unsigned i=0;i<sizeof funcs/sizeof funcs[0];++i)
            if (strlen(funcs[i].name)==len && !memcmp(name,funcs[i].name,len)) return funcs[i].fn(a);
        f->error=1; return 0;
    }
    char *end;
    double v=strtod(f->s,&end);
    if (end==f->s) f->error=1;
    f->s=end; return v;
}
static double factor(Formula *f) {
    if (take(f,'-')) return -factor(f);
    if (take(f,'+')) return factor(f);
    double v=atom(f);
    if (take(f,'^')) v=pow(v,factor(f));
    return v;
}
static double product(Formula *f) {
    double v=factor(f);
    while (!f->error) {
        if (take(f,'*')) v*=factor(f);
        else if (take(f,'/')) v/=factor(f);
        else break;
    }
    return v;
}
static double sum_expr(Formula *f) {
    double v=product(f);
    while (!f->error) {
        if (take(f,'+')) v+=product(f);
        else if (take(f,'-')) v-=product(f);
        else break;
    }
    return v;
}
static int formula_value(const char *s,double t,double *out) {
    Formula f={s,t,0};
    double v=sum_expr(&f);
    spaces(&f);
    if (*f.s || f.error || !isfinite(v) || fabs(v)>10000) return 0;
    *out=v; return 1;
}
