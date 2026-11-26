// Guest regression for shared memory, preemption, TLS and musl thread lifetime.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <fenv.h>
#include "../console.h"

static _Thread_local int local = 17;
static atomic_int ready, go, detached;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int sent, received, child_cpu, child_tid, child_pid;
static void *worker(void *arg) {
    child_cpu = console_call(29,10);
    child_tid = syscall(SYS_gettid);
    child_pid = getpid();
    int initial=local;
    int rounding=fegetround();
    fesetround(FE_TOWARDZERO);
    local = 93;
    atomic_store(&ready,1);
    while (!atomic_load(&go)) {}
    pthread_mutex_lock(&lock);
    while (!sent) pthread_cond_wait(&cond,&lock);
    received = 1;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
    return rounding == FE_DOWNWARD && initial == 17 && local == 93 ? arg : NULL;
}
static void *brief(void *arg) { atomic_fetch_add(&detached,1); return arg; }
static void *spin(void *arg) { atomic_store(&ready,1); for (;;) {} }
static void *finish_main(void *arg) { usleep(10000); puts("survived main pthread_exit"); return NULL; }
static void *read_pipe(void *arg) {
    int *pair=arg;
    char value=0;
    atomic_store(&ready,1);
    return read(pair[0],&value,1)==1 && value=='x' ? arg : NULL;
}
static void *hold_slot(void *arg) {
    pthread_mutex_lock(&lock);
    while (!sent) pthread_cond_wait(&cond,&lock);
    pthread_mutex_unlock(&lock);
    return arg;
}
int main(int argc, char **argv) {
    pthread_t t;
    if (console_call(61,0)!=1) return 1;
    if (argc>1 && !strcmp(argv[1],"exit-group")) {
        if (pthread_create(&t,NULL,spin,NULL)) return 2;
        while (!atomic_load(&ready)) {}
        return 23;
    }
    if (argc>1 && !strcmp(argv[1],"exit-main")) {
        if (pthread_create(&t,NULL,finish_main,NULL)) return 3;
        pthread_exit(NULL);
    }
    int main_tid=syscall(SYS_gettid), cpu=console_call(29,10);
    local=42;
    fesetround(FE_DOWNWARD);
    if (pthread_create(&t,NULL,worker,&ready)) return 4;
    while (!atomic_load(&ready)) {}
    atomic_store(&go,1);
    pthread_mutex_lock(&lock);
    sent=1;
    pthread_cond_signal(&cond);
    while (!received) pthread_cond_wait(&cond,&lock);
    pthread_mutex_unlock(&lock);
    void *answer=NULL;
    if (pthread_join(t,&answer) || answer!=&ready || local!=42 || fegetround()!=FE_DOWNWARD ||
        getpid()!=child_pid || main_tid!=child_pid || main_tid==child_tid) return 5;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC,&deadline);
    deadline.tv_nsec+=20000000;
    if (deadline.tv_nsec>=1000000000) { deadline.tv_sec++; deadline.tv_nsec-=1000000000; }
    int futex=0;
    if (syscall(SYS_futex,&futex,9,0,&deadline,0,~0u)!=-1 || errno!=ETIMEDOUT) return 6;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    for (int i=0;i<32;i++) {
        if (pthread_create(&t,&attr,brief,NULL)) return 7;
        while (atomic_load(&detached)<=i) {}
        if (pthread_create(&t,NULL,brief,&ready) || pthread_join(t,&answer) || answer!=&ready) return 8;
        atomic_fetch_sub(&detached,1);
    }
    pthread_attr_destroy(&attr);
    int pair[2];
    atomic_store(&ready,0);
    if (pipe(pair) || pthread_create(&t,NULL,read_pipe,pair)) return 9;
    while (!atomic_load(&ready)) {}
    usleep(5000);
    if (write(pair[1],"x",1)!=1 || pthread_join(t,&answer) || answer!=pair) return 10;
    close(pair[0]);
    close(pair[1]);
    pthread_t held[64];
    int count=0, error;
    sent=0;
    while (count<64 && !(error=pthread_create(&held[count],NULL,hold_slot,NULL))) count++;
    if (count<16 || count==64 || error!=EAGAIN) return 11;
    pthread_mutex_lock(&lock);
    sent=1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    for (int i=0;i<count;i++) if (pthread_join(held[i],NULL)) return 12;
    printf("pthread OK pid=%d tids=%d,%d cores=%d,%d detached=32 simultaneous=%d\n",child_pid,main_tid,child_tid,cpu,child_cpu,count);
    return 0;
}
