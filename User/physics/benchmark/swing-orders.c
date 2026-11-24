/* Host benchmark. Build: cc -O3 -std=c11 swing-orders.c -lm -o swing-orders
 * Run: ./swing-orders ORDER SEED [MAX_GENERATIONS [WEIGHTS_FILE]]. One CSV row on stdout. */
#include "../order-swing.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { PARAMS=SWING_FEATURES*MAX_ORDER, POP=56, ELITE=9, STEPS=960 };
typedef struct { double w[PARAMS], reward; } Candidate;
typedef struct { double reward; int held; } Result;

static double uniform(uint32_t *rng) {
    *rng ^= *rng << 13;
    *rng ^= *rng >> 17;
    *rng ^= *rng << 5;
    return (*rng + .5) / 4294967296.;
}
static double normal(uint32_t *rng) {
    double u=uniform(rng);
    return sqrt(-2*log(u))*cos(2*swing_pi*uniform(rng));
}
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec*1e-9;
}
static int descending(const void *a,const void *b) {
    double x=((const Candidate *)a)->reward,y=((const Candidate *)b)->reward;
    return (x<y)-(x>y);
}
static Result episode(int n,const double *tilt,const double *w) {
    Mechanism m;
    SwingMotors motors;
    setup_swing(&m,&motors,n,tilt);
    Result r={0};
    for (int t=0;t<STEPS;++t) {
        double cost=advance_swing(&m,&motors,w);
        int stable=swing_is_balanced(&m,&motors);
        r.held=stable?r.held+1:0;
        if (t%4==3) {
            double align=0,speed=0;
            for (int i=0;i<n;++i) {
                Link *l=&m.links[i];
                align-=cos(motors.rest[i]+l->angle);
                speed+=fmin(36,l->omega*l->omega);
            }
            r.reward+=(1+t/(double)STEPS)*(align+stable*.9-speed*.004-cost*.006);
        }
    }
    return r;
}
static int evaluate(int n,const double *w,uint32_t rng,int count) {
    int passed=0;
    for (int j=0;j<count;++j) {
        double tilt[MAX_ORDER];
        for (int i=0;i<MAX_ORDER;++i) tilt[i]=(2*uniform(&rng)-1)*10*swing_pi/180;
        passed+=episode(n,tilt,w).held>=240;
    }
    return passed;
}
int main(int argc,char **argv) {
    if (argc<3 || argc>5) return 2;
    int n=atoi(argv[1]),budget=argc>=4?atoi(argv[3]):50;
    uint32_t seed=(uint32_t)strtoul(argv[2],NULL,0),rng=seed;
    if (n<3 || n>MAX_ORDER || !seed || budget<1) return 2;
    double start=now(),down[MAX_ORDER]={0},mean[PARAMS]={0},sd[PARAMS];
    Candidate pop[POP]={0},champion={0};
    int dim=n*SWING_FEATURES;
    for (int i=0;i<dim;++i) {
        int k=i%SWING_FEATURES;
        mean[i]=k==0?1.3:k==1?.4:0;
        sd[i]=k==0?1:k==1?.35:k==3||k==5?.17:.55;
    }
    memcpy(champion.w,mean,sizeof mean);
    Result nominal=episode(n,down,champion.w);
    champion.reward=nominal.reward;
    int gen=0,valid=-1,passes=0;
    for (;;) {
        if (nominal.held>=240 && valid<0)
            valid=evaluate(n,champion.w,0x86c18b37,16);
        passes=nominal.held>=240 && valid>=15;
        if (passes || gen==budget) break;
        ++gen;
        double tilt[MAX_ORDER];
        for (int i=0;i<n;++i) tilt[i]=(uniform(&rng)-.5)*.18;
        for (int p=0;p<POP;++p) {
            for (int i=0;i<dim;++i)
                pop[p].w[i]=p?mean[i]+sd[i]*normal(&rng):mean[i];
            pop[p].reward=episode(n,tilt,pop[p].w).reward;
        }
        qsort(pop,POP,sizeof *pop,descending);
        for (int i=0;i<dim;++i) {
            double avg=0,var=0;
            for (int p=0;p<ELITE;++p) avg+=pop[p].w[i]/ELITE;
            for (int p=0;p<ELITE;++p) {
                double d=pop[p].w[i]-avg;
                var+=d*d/ELITE;
            }
            mean[i]=.25*mean[i]+.75*avg;
            sd[i]=fmax(.018,.25*sd[i]+.75*sqrt(var));
        }
        Result test=episode(n,down,pop[0].w);
        if (test.reward>champion.reward) {
            champion=pop[0]; champion.reward=test.reward;
            nominal=test; valid=-1;
        }
    }
    double elapsed=now()-start;
    /* The independent test bank is opened once, after stopping. */
    if (valid<0) valid=evaluate(n,champion.w,0x86c18b37,16);
    int heldout=evaluate(n,champion.w,0xf23a9cd1,32);
    if (argc==5) {
        FILE *out=fopen(argv[4],"w");
        if (!out) { perror(argv[4]); return 1; }
        for (int i=0;i<dim;++i)
            fprintf(out,"%.17g%c",champion.w[i],(i+1)%SWING_FEATURES?' ':'\n');
        if (fclose(out)) { perror(argv[4]); return 1; }
    }
    printf("%d,%u,%d,%d,%d,%d,%.3f,%d,%d,%.3f\n",n,seed,budget,
           passes,gen,gen*POP,elapsed,valid,heldout,nominal.held/120.);
    return 0;
}
