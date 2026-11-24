#pragma once
#include "examples.h"

/* Three bounded revolute-joint motors. Each action is a torque in N m.
 * The policy reads the absolute tilt and speed of all three rods and outputs
 * equal/opposite torques on the two bodies attached to each joint. */
enum { SWING_FEATURES=7, SWING_PARAMS=3*SWING_FEATURES };
static const double swing_pi=3.14159265358979323846;
static const double torque_limit[]={16,8,3};

static inline void hanging_scene(Mechanism *m,const double tilt[3]) {
    memset(m,0,sizeof *m);
    m->gravity=vec(0,-9.8);
    Vec p=vec(0,0);
    for (int i=0;i<3;++i) {
        Vec end=add(p,mul(vec(sin(tilt[i]),-cos(tilt[i])),0.65));
        Vec rod[]={p,end};
        add_link(m,rod,2);
        add_joint(m,REVOLUTE,i-1,i,p,vec(0,0));
        p=end;
    }
}
static double swing_angle(const Link *l) {
    Vec axis=sub(l->point[1],l->point[0]);
    return atan2(axis.x,-axis.y)+l->angle;
}
static inline double swing_action(const Mechanism *m,const double weights[SWING_PARAMS],double torque[3]) {
    double error[3],velocity[3],cost=0;
    for (int i=0;i<3;++i) {
        error[i]=swing_pi-swing_angle(&m->links[i]);
        velocity[i]=m->links[i].omega;
    }
    for (int i=0;i<3;++i) {
        int next=(i+1)%3,other=(i+2)%3;
        double own=i ? error[i]-error[i-1] : error[0];
        double speed=i ? velocity[i]-velocity[i-1] : velocity[0];
        double f[]={own,-speed,error[next],-velocity[next],error[other],-velocity[other],sin(error[0])};
        const double *w=weights+i*SWING_FEATURES;
        double value=0;
        for (int j=0;j<SWING_FEATURES;++j) value+=w[j]*f[j];
        double command=fmin(1,fmax(-1,value));
        torque[i]=torque_limit[i]*command;
        cost+=command*command;
    }
    return cost;
}
static inline void motor_step(Mechanism *m,const double torque[3],double dt) {
    for (int i=0;i<3;++i) {
        const Joint *j=&m->joints[i];
        Link *child=&m->links[j->b];
        child->omega+=torque[i]*child->inv_inertia*dt;
        if (j->a!=WORLD) {
            Link *parent=&m->links[j->a];
            parent->omega-=torque[i]*parent->inv_inertia*dt;
        }
    }
    physics_step(m,dt);
}
static inline int swing_upright(const Mechanism *m) {
    for (int i=0;i<3;++i) {
        double angle=remainder(swing_angle(&m->links[i])-swing_pi,2*swing_pi);
        if (!(fabs(angle)<=5*swing_pi/180 && fabs(m->links[i].omega)<=0.20)) return 0;
    }
    return 1;
}
