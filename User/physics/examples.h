#pragma once
#include "mechanism.h"

enum PendulumExample { SINGLE_PENDULUM, TRIPLE_INVERTED };
static void pendulum_example(Mechanism *m,enum PendulumExample example) {
    memset(m,0,sizeof *m);
    m->gravity=vec(0,-9.8);
    if (example==SINGLE_PENDULUM) {
        Vec rod[]={vec(-1.25,.90),vec(.35,-.05)};
        add_link(m,rod,2);
        add_joint(m,REVOLUTE,WORLD,0,rod[0],vec(0,0));
        return;
    }
    /* Absolute angles from upward vertical. Each distal end is the next hinge. */
    const double degrees[]={3,6,9};
    Vec pivot=vec(0,0);
    int base=WORLD;
    for (int i=0;i<3;++i) {
        double angle=degrees[i]*3.141592653589793/180;
        Vec end=add(pivot,mul(vec(sin(angle),cos(angle)),0.65));
        Vec rod[]={pivot,end};
        int child=add_link(m,rod,2);
        add_joint(m,REVOLUTE,base,child,pivot,vec(0,0));
        base=child;
        pivot=end;
    }
}
