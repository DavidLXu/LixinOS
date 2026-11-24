#include "order-swing.h"
#include "swing-weights.h"
#include "swing-presets.h"
#include "expression.h"
#include <assert.h>
#include <stdio.h>
static void close_to(double a,double b,double tolerance) { assert(fabs(a-b)<tolerance); }
static Mechanism m;
static int rod(Vec a,Vec b) { Vec p[]={a,b}; return add_link(&m,p,2); }
static void advance(int n) { while (n--) physics_step(&m,1.0/120); }
/* Every frozen policy must be playable from rest and replay the same
 * trajectory after reset, including the high-order policies that fall. */
static void test_presets(void) {
    int counts[MAX_ORDER+1]={0};
    const double down[MAX_ORDER]={0};
    for (size_t p=0;p<sizeof swing_presets/sizeof *swing_presets;++p) {
        const SwingPreset *preset=&swing_presets[p];
        SwingMotors motors;
        setup_swing(&m,&motors,preset->order,down);
        assert(m.link_count==preset->order && m.joint_count==preset->order);
        ++counts[preset->order];
        int held=0;
        double first_tip=0;
        for (int t=0;t<960;++t) {
            advance_swing(&m,&motors,preset->weights);
            for (int i=0;i<m.link_count;++i) {
                assert(isfinite(m.links[i].angle) && isfinite(m.links[i].omega));
                assert(fabs(motors.torque[i])<=motors.cap[i]);
            }
            held=swing_is_balanced(&m,&motors)?held+1:0;
            if (!t) first_tip=m.links[preset->order-1].angle;
        }
        if (preset->test_successes==32) assert(held>=240);
        rewind_scene(&m);
        advance_swing(&m,&motors,preset->weights);
        assert(m.links[preset->order-1].angle==first_tip);
    }
    for (int n=3;n<=MAX_ORDER;++n) assert(counts[n]==(n==3?4:n<7?3:6));
}
int main(void) {
    double y;
    assert(formula_value("sin(pi/2) + -2^2 + 2^-2 + x",0.5,&y));
    close_to(y,-2.25,1e-12);
    assert(formula_value("2^3^2",0,&y)); close_to(y,512,1e-12);
    assert(!formula_value("sqrt(-1)",1,&y) && !formula_value("1/(t-1)",1,&y));
    assert(!formula_value("1+x)",1,&y) && !formula_value("bad(t)",0,&y));
    Vec arc[CURVE_POINTS];
    assert(arc_points(vec(1,0),vec(0,1),vec(-1,0),arc)==CURVE_POINTS);
    close_to(arc[48].y,1,1e-12); close_to(arc[96].x,-1,1e-12);
    arc_points(vec(1,0),vec(0,-1),vec(0,1),arc);
    close_to(arc[32].y,-1,1e-12); close_to(arc[96].y,1,1e-12);
    assert(!arc_points(vec(1,0),vec(2,0),vec(3,0),arc));

    m.gravity=vec(0,-9.8);
    int b=rod(vec(0,0),vec(2,0)); assert(b==0);
    close_to(m.links[0].inv_inertia,1.5,1e-12);
    advance(120); close_to(m.links[0].velocity.y,-9.8,1e-8);
    close_to(m.links[0].center.y,-4.9,0.05);
    rewind_scene(&m);
    assert(add_joint(&m,PRISMATIC,WORLD,b,vec(0.5,0),vec(1,-1)));
    advance(120);
    close_to(m.links[0].angle,0,1e-8);
    close_to(m.links[0].velocity.x,4.9,1e-7);
    close_to(m.links[0].center.x-1,-m.links[0].center.y,1e-7);
    rewind_scene(&m); m.joint_count=0;
    assert(add_joint(&m,REVOLUTE,WORLD,b,vec(0,0),vec(0,0)));
    int c=rod(vec(2,0),vec(2,-1));
    assert(add_joint(&m,REVOLUTE,b,c,vec(2,0),vec(0,0)));
    advance(80);
    Vec anchor=world_point(&m.links[0],m.joints[0].anchor_b);
    Vec p=world_point(&m.links[0],m.joints[1].anchor_a);
    Vec q=world_point(&m.links[1],m.joints[1].anchor_b);
    close_to(dot(anchor,anchor),0,1e-6);
    close_to(dot(sub(p,q),sub(p,q)),0,1e-6); assert(m.links[0].angle < -0.3);
    rewind_scene(&m); m.joint_count=1;
    assert(add_joint(&m,PRISMATIC,b,c,vec(2,-0.5),vec(1,0)));
    advance(100);
    Joint j=m.joints[1];
    p=world_point(&m.links[b],j.anchor_a); q=world_point(&m.links[c],j.anchor_b);
    Vec normal=perp(rotate(j.axis_a,m.links[b].angle));
    close_to(dot(sub(q,p),normal),0,1e-4);
    close_to(m.links[b].angle,m.links[c].angle,1e-4);
    rewind_scene(&m); close_to(m.links[c].center.y,-0.5,1e-12);
    pendulum_example(&m,TRIPLE_INVERTED);
    assert(m.link_count==3 && m.joint_count==3);
    Vec initial_tip=world_point(&m.links[2],m.links[2].point[1]);
    assert(initial_tip.y>1.9);
    int fell=0;
    for (int t=0;t<1200;++t) {
        physics_step(&m,1.0/120);
        for (int i=0;i<3;++i) {
            Joint *hinge=&m.joints[i];
            assert(hinge->type==REVOLUTE && hinge->a==i-1 && hinge->b==i);
            Vec base=i?world_point(&m.links[i-1],hinge->anchor_a):vec(0,0);
            Vec gap=sub(world_point(&m.links[i],hinge->anchor_b),base);
            assert(hypot(gap.x,gap.y)<0.0005);
        }
        Vec tip=world_point(&m.links[2],m.links[2].point[1]);
        if (tip.y<0) fell=1;
    }
    assert(fell);
    rewind_scene(&m);
    Vec restored_tip=world_point(&m.links[2],m.links[2].point[1]);
    close_to(restored_tip.y,initial_tip.y,1e-12);
    pendulum_example(&m,SINGLE_PENDULUM);
    assert(m.link_count==1 && m.joint_count==1 && m.joints[0].a==WORLD);
    const double down[]={0,0,0};
    hanging_scene(&m,down);
    for (int step=0;step<120*60;++step) {
        double motor[3];
        swing_action(&m,swing_weights,motor);
        for (int i=0;i<3;++i) assert(fabs(motor[i])<=torque_limit[i]);
        motor_step(&m,motor,1.0/120);
        if (step>=2*120) assert(swing_upright(&m));
    }
    rewind_scene(&m); assert(!swing_upright(&m));
    test_presets();
    puts("PASS all 37 policy rollouts and deterministic reset");
    puts("PASS formulas, arcs, mass/inertia, gravity, fixed and moving revolute/prismatic joints");
}
