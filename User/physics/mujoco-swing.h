#pragma once
#include <mujoco/mujoco.h>
#include "order-swing.h"
#include "triple-model.h"

typedef struct { mjModel *model; mjData *data; } MujocoSwing;

static void close_mujoco_swing(MujocoSwing *s) {
    if (s->data) mj_deleteData(s->data);
    if (s->model) mj_deleteModel(s->model);
    *s=(MujocoSwing){0};
}

static int open_mujoco_swing(MujocoSwing *s) {
    if (s->data) { mj_resetData(s->model,s->data); return 1; }
    s->model=mj_loadModelBuffer(triple_mjb,sizeof triple_mjb);
    if (s->model) s->data=mj_makeData(s->model);
    if (!s->data) { close_mujoco_swing(s); return 0; }
    return 1;
}

static void advance_mujoco_swing(MujocoSwing *s,Mechanism *scene,SwingMotors *motors,const double *weights) {
    mjData *d=s->data;
    swing_action(scene,weights,motors->torque);
    for (int i=0;i<3;++i) d->ctrl[i]=motors->torque[i];
    s->model->opt.gravity[0]=scene->gravity.x;
    s->model->opt.gravity[2]=scene->gravity.y;
    for (int tick=0;tick<4;++tick) mj_step(s->model,d);
    /* Joint coordinates are relative to the parent. Preserve complete turns
     * when reconstructing the absolute angles observed by the trained policy. */
    double angle=0,omega=0;
    Vec pivot=vec(0,0);
    for (int i=0;i<3;++i) {
        Link *l=&scene->links[i];
        angle+=d->qpos[i]; omega+=d->qvel[i];
        Vec half=rotate(vec(0,-0.325),angle);
        l->center=add(pivot,half);
        pivot=add(l->center,half);
        l->angle=angle; l->omega=omega;
    }
    scene->time=d->time;
}
