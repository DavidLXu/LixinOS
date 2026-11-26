#ifndef LX_KTHREAD_H
#define LX_KTHREAD_H
#define KT_TASK_SLOTS 64
#define KT_LEGACY_THREADS 16
#define KT_PROCESS_THREADS 64
/* SVC 61 takes an eight-word request on the calling thread's stack. */
enum { KT_SELF, KT_CREATE, KT_CLEAR_TID, KT_WAIT, KT_WAKE, KT_REQUEUE,
       KT_YIELD, KT_COUNT, KT_LEADER_EXIT, KT_WORKER, KT_INDEX };
#define KT_THREAD_DONE 512
long kt_call(unsigned long request);
unsigned kt_owner(unsigned slot);
long kt_run(unsigned slot);
long kt_end_group(long status);
unsigned kt_worker(unsigned slot);
void kt_service_enter(void);
void kt_service_leave(void);
#endif
