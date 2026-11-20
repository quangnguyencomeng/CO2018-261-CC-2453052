#ifndef SCHED_H
#define SCHED_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MLQ_SCHED
#define MLQ_SCHED
#endif

#define MAX_PRIO 140

enum sched_add_result {
	SCHED_ADD_INVALID = -1,
	SCHED_ADD_FULL = 0,
	SCHED_ADD_OK = 1
};

int queue_empty(void);

void init_scheduler(struct krnl_t *kernel);
void finish_scheduler(void);
void remove_proc_from_running_list(struct pcb_t *proc);

/* Get the next process from ready queue */
struct pcb_t * get_proc(void);

/* Put a process back to run queue */
int put_proc(struct pcb_t * proc);

/* Add a new process to ready queue */
enum sched_add_result add_proc(struct pcb_t * proc);

#ifdef __cplusplus
}
#endif

#endif
