#pragma once
#include <math.h>
#include <string.h>

enum { MAX_LINKS = 40, MAX_JOINTS = 80, CURVE_POINTS = 97, WORLD = -1 };
typedef struct { double x, y; } Vec;
typedef struct {
    Vec point[CURVE_POINTS], rest, center, velocity;
    int count;
    double angle, omega, inv_mass, inv_inertia;
} Link;
enum JointType { REVOLUTE, PRISMATIC };
typedef struct {
    enum JointType type;
    int a, b;
    Vec anchor_a, anchor_b, axis_a;
    double reference;
} Joint;
typedef struct {
    Link links[MAX_LINKS];
    Joint joints[MAX_JOINTS];
    int link_count, joint_count;
    Vec gravity;
    double time;
} Mechanism;

static Vec vec(double x,double y) { return (Vec){x,y}; }
static Vec add(Vec a,Vec b) { return vec(a.x+b.x,a.y+b.y); }
static Vec sub(Vec a,Vec b) { return vec(a.x-b.x,a.y-b.y); }
static Vec mul(Vec a,double k) { return vec(a.x*k,a.y*k); }
static double dot(Vec a,Vec b) { return a.x*b.x+a.y*b.y; }
static double cross(Vec a,Vec b) { return a.x*b.y-a.y*b.x; }
static Vec perp(Vec a) { return vec(-a.y,a.x); }
static Vec rotate(Vec a,double angle) {
    double c=cos(angle), s=sin(angle);
    return vec(a.x*c-a.y*s,a.x*s+a.y*c);
}
static Vec world_point(const Link *l,Vec local) { return add(l->center,rotate(local,l->angle)); }
static Vec local_point(const Link *l,Vec p) { return rotate(sub(p,l->center),-l->angle); }

/* Uniform slender rod per chord. Integrate its centroid and polar moment,
 * including the intrinsic L^2/12 contribution of each segment. */
static int add_link(Mechanism *m,const Vec *points,int n) {
    if (m->link_count >= MAX_LINKS || n < 2 || n > CURVE_POINTS) return -1;
    Vec weighted={0,0};
    double mass=0, inertia=0;
    for (int i=1;i<n;++i) {
        Vec delta=sub(points[i],points[i-1]);
        double length=sqrt(dot(delta,delta));
        mass+=length;
        weighted=add(weighted,mul(add(points[i],points[i-1]),0.5*length));
    }
    if (!isfinite(mass) || mass<0.01 || mass>1e4) return -1;
    Vec center=mul(weighted,1/mass);
    for (int i=1;i<n;++i) {
        Vec delta=sub(points[i],points[i-1]);
        double l2=dot(delta,delta);
        Vec offset=sub(mul(add(points[i],points[i-1]),0.5),center);
        inertia+=sqrt(l2)*(l2/12+dot(offset,offset));
    }
    if (!isfinite(inertia) || inertia<=0) return -1;
    Link *l=&m->links[m->link_count];
    memset(l,0,sizeof *l);
    l->count=n; l->center=l->rest=center;
    l->inv_mass=1/mass; l->inv_inertia=1/inertia;
    for (int i=0;i<n;++i) l->point[i]=sub(points[i],center);
    return m->link_count++;
}
static int arc_points(Vec p,Vec middle,Vec end,Vec *out) {
    Vec a=sub(middle,p), b=sub(end,p);
    double den=2*cross(a,b);
    if (fabs(den)<1e-7) return 0;
    Vec c=add(p,mul(sub(mul(perp(a),dot(b,b)),mul(perp(b),dot(a,a))),1/den));
    double first=atan2(p.y-c.y,p.x-c.x), finish=atan2(end.y-c.y,end.x-c.x);
    double sweep=finish-first, tau=6.283185307179586;
    if (den>0 && sweep<=0) sweep+=tau;
    if (den<0 && sweep>=0) sweep-=tau;
    Vec radius=sub(p,c);
    for (int i=0;i<CURVE_POINTS;++i) out[i]=add(c,rotate(radius,sweep*i/(CURVE_POINTS-1)));
    return CURVE_POINTS;
}
static int add_joint(Mechanism *m,enum JointType type,int a,int b,Vec pivot,Vec axis) {
    if (m->joint_count>=MAX_JOINTS || a<WORLD || b<0 || a>=m->link_count || b>=m->link_count || a==b) return 0;
    double len=sqrt(dot(axis,axis));
    if (type==PRISMATIC && len<0.01) return 0;
    Link *base=a==WORLD ? NULL : &m->links[a];
    Link *body=&m->links[b];
    m->joints[m->joint_count++]=(Joint){type,a,b,base?local_point(base,pivot):pivot,
        local_point(body,pivot),type==PRISMATIC?rotate(mul(axis,1/len),base?-base->angle:0):vec(0,0),
        body->angle-(base?base->angle:0)};
    return 1;
}
static void project_joint(Mechanism *m,const Joint *j) {
    Link *a=j->a==WORLD ? NULL : &m->links[j->a], *b=&m->links[j->b];
    double ma=a?a->inv_mass:0, ia=a?a->inv_inertia:0, mb=b->inv_mass, ib=b->inv_inertia;
    if (j->type==PRISMATIC) {
        double angle_error=b->angle-(a?a->angle:0)-j->reference;
        double impulse=-angle_error/(ia+ib);
        if (a) a->angle-=ia*impulse;
        b->angle+=ib*impulse;
    }
    Vec ra=a?rotate(j->anchor_a,a->angle):vec(0,0), rb=rotate(j->anchor_b,b->angle);
    Vec delta=sub(add(b->center,rb),a?add(a->center,ra):j->anchor_a);
    Vec impulse;
    double ta,tb;
    if (j->type==REVOLUTE) {
        double xx=ma+mb+ia*ra.y*ra.y+ib*rb.y*rb.y;
        double yy=ma+mb+ia*ra.x*ra.x+ib*rb.x*rb.x;
        double xy=-ia*ra.x*ra.y-ib*rb.x*rb.y, det=xx*yy-xy*xy;
        impulse=vec((xy*delta.y-yy*delta.x)/det,(xy*delta.x-xx*delta.y)/det);
        ta=-cross(ra,impulse); tb=cross(rb,impulse);
    } else {
        Vec axis=rotate(j->axis_a,a?a->angle:0), normal=perp(axis);
        double ga=-cross(ra,normal)-dot(axis,delta), gb=cross(rb,normal);
        double amount=-dot(normal,delta)/(ma+mb+ia*ga*ga+ib*gb*gb);
        impulse=mul(normal,amount); ta=ga*amount; tb=gb*amount;
    }
    if (a) { a->center=sub(a->center,mul(impulse,ma)); a->angle+=ia*ta; }
    b->center=add(b->center,mul(impulse,mb)); b->angle+=ib*tb;
}
static void physics_step(Mechanism *m,double dt) {
    Vec previous[MAX_LINKS];
    double angle[MAX_LINKS];
    for (int i=0;i<m->link_count;++i) {
        Link *l=&m->links[i];
        previous[i]=l->center; angle[i]=l->angle;
        l->velocity=add(l->velocity,mul(m->gravity,dt));
        l->center=add(l->center,mul(l->velocity,dt));
        l->angle+=l->omega*dt;
    }
    for (int pass=0;pass<12;++pass)
        for (int i=0;i<m->joint_count;++i) project_joint(m,&m->joints[i]);
    for (int i=0;i<m->link_count;++i) {
        Link *l=&m->links[i];
        l->velocity=mul(sub(l->center,previous[i]),1/dt);
        l->omega=(l->angle-angle[i])/dt;
    }
    m->time+=dt;
}
static void rewind_scene(Mechanism *m) {
    for (int i=0;i<m->link_count;++i) {
        Link *l=&m->links[i];
        l->center=l->rest; l->angle=l->omega=0; l->velocity=vec(0,0);
    }
    m->time=0;
}
