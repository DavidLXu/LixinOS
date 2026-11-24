#include <mujoco/mujoco.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mjModel *load_model(const char *path) {
    FILE *file=fopen(path,"rb");
    if (!file) { perror(path); return NULL; }
    if (fseek(file,0,SEEK_END)) { fclose(file); return NULL; }
    long size=ftell(file);
    if (size<=0 || fseek(file,0,SEEK_SET)) { fclose(file); return NULL; }
    void *bytes=malloc((size_t)size);
    mjModel *model=NULL;
    if (bytes && fread(bytes,1,(size_t)size,file)==(size_t)size)
        model=mj_loadModelBuffer(bytes,(mjtSize)size);
    free(bytes);
    fclose(file);
    return model;
}

int main(int argc,char **argv) {
    const char *path=argc>1?argv[1]:"/lib/mujoco/pendulum.mjb";
    long steps=1000;
    if (argc>2) {
        char *end;
        errno=0;
        steps=strtol(argv[2],&end,10);
        if (errno || end==argv[2] || *end || steps<1 || steps>10000000) steps=0;
    }
    if (argc>3 || !steps) { fputs("usage: mujoco [model.mjb [steps]]\n",stderr); return 2; }
    mjModel *m=load_model(path);
    if (!m) { fprintf(stderr,"cannot load MuJoCo %s MJB: %s\n",mj_versionString(),path); return 1; }
    if (m->nplugin) { fputs("model requires external plugins\n",stderr); mj_deleteModel(m); return 1; }
    mjData *d=mj_makeData(m);
    if (!d) { mj_deleteModel(m); return 1; }
    if (m->nkey) mj_resetDataKeyframe(m,d,0);
    printf("MuJoCo %s nq=%ld nv=%ld dt=%.6f\n",mj_versionString(),(long)m->nq,(long)m->nv,m->opt.timestep);
    for (long step=0;step<=steps;++step) {
        if (step==0 || step==steps/2 || step==steps)
            printf("step=%ld time=%.9f q0=%.12f v0=%.12f\n",step,d->time,
                   m->nq?d->qpos[0]:0.,m->nv?d->qvel[0]:0.);
        if (step<steps) mj_step(m,d);
    }
    int failed=!isfinite(d->time) || fabs(d->time-steps*m->opt.timestep)>1e-8*(1+fabs(d->time));
    for (mjtSize i=0;i<m->nq;++i) failed |= !isfinite(d->qpos[i]);
    for (mjtSize i=0;i<m->nv;++i) failed |= !isfinite(d->qvel[i]);
    mj_deleteData(d); mj_deleteModel(m);
    return failed;
}
