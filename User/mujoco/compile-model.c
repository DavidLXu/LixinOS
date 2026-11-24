/* Host-side MJCF compiler for the matching upstream MuJoCo library. */
#include <mujoco/mujoco.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc,char **argv) {
    if (argc!=3) { fputs("usage: compile-model source.xml output.mjb\n",stderr); return 2; }
    char error[1024]={0};
    mjModel *m=mj_loadXML(argv[1],NULL,error,sizeof error);
    if (!m) { fprintf(stderr,"%s\n",error); return 1; }
    mjtSize size=mj_sizeModel(m);
    void *bytes=malloc(size);
    if (!bytes) { mj_deleteModel(m); return 1; }
    mj_saveModel(m,NULL,bytes,size);
    mj_deleteModel(m);
    FILE *f=fopen(argv[2],"wb");
    if (!f) { perror(argv[2]); free(bytes); return 1; }
    int failed=fwrite(bytes,1,size,f)!=size;
    failed |= fclose(f)!=0;
    free(bytes);
    return failed;
}
