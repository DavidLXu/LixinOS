#pragma once
#include "swing.h"

/* Benchmarked bounded feedback policy, shared by training and Win3 playback. */
enum { MAX_ORDER=10 };
typedef struct {
    double rest[MAX_ORDER], cap[MAX_ORDER], torque[MAX_ORDER];
} SwingMotors;
typedef struct {
    int order, seed_number, generations, test_successes;
    const double *weights;
} SwingPreset;
static void setup_swing(Mechanism *m,SwingMotors *state,int n,const double *tilt) {
    memset(m,0,sizeof *m);
    memset(state,0,sizeof *state);
    m->gravity=vec(0,-9.8);
    Vec p=vec(0,0);
    for (int i=0;i<n;++i) {
        Vec end=add(p,mul(vec(sin(tilt[i]),-cos(tilt[i])),.65));
        Vec points[]={p,end};
        add_link(m,points,2);
        add_joint(m,REVOLUTE,i-1,i,p,vec(0,0));
        state->rest[i]=swing_angle(&m->links[i]);
        /* Extend the original 16, 8, 3 Nm limits by distal link count. */
        int k=n-i;
        state->cap[i]=1.5*k*k+.5*k+1;
        p=end;
    }
}

static double advance_swing(Mechanism *m,SwingMotors *state,const double *w) {
    int n=m->link_count;
    double e[MAX_ORDER],v[MAX_ORDER],cost=0;
    for (int i=0;i<n;++i) {
        e[i]=swing_pi-(state->rest[i]+m->links[i].angle);
        v[i]=m->links[i].omega;
    }
    double root_feature=sin(e[0]);
    for (int i=0;i<n;++i) {
        int next=(i+1)%n,other=(i+2)%n;
        double f[]={i?e[i]-e[i-1]:e[i],i?v[i-1]-v[i]:-v[i],
                    e[next],-v[next],e[other],-v[other],root_feature};
        double raw=0;
        for (int j=0;j<SWING_FEATURES;++j) raw+=w[i*SWING_FEATURES+j]*f[j];
        double command=fmin(1,fmax(-1,raw));
        state->torque[i]=state->cap[i]*command;
        double impulse=state->torque[i]/120.;
        m->links[i].omega+=impulse*m->links[i].inv_inertia;
        if (i) m->links[i-1].omega-=impulse*m->links[i-1].inv_inertia;
        cost+=command*command;
    }
    physics_step(m,1./120);
    return cost;
}

static int swing_is_balanced(const Mechanism *m,const SwingMotors *state) {
    int stable=1;
    for (int i=0;i<m->link_count && stable;++i) {
        const Link *l=&m->links[i];
        double angle=remainder(state->rest[i]+l->angle-swing_pi,2*swing_pi);
        stable=fabs(angle)<=5*swing_pi/180 && fabs(l->omega)<=.20;
        if (stable) {
            const Joint *j=&m->joints[i];
            Vec parent=i?world_point(&m->links[i-1],j->anchor_a):j->anchor_a;
            Vec gap=sub(parent,world_point(l,j->anchor_b));
            stable=dot(gap,gap)<=.01*.01;
        }
    }
    return stable;
}
