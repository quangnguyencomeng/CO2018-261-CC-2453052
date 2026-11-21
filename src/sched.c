/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "queue.h"
#include "sched.h"
#include <pthread.h>

#include <stdlib.h>
#include <stdio.h>
static struct queue_t ready_queue;
static struct queue_t run_queue;
static pthread_mutex_t queue_lock;

static struct queue_t running_list;
#ifdef MLQ_SCHED
static struct queue_t mlq_ready_queue[MAX_PRIO];
static int slot[MAX_PRIO];
#endif

int queue_empty(void) {
	int result;

	pthread_mutex_lock(&queue_lock);
#ifdef MLQ_SCHED
	int prio;
	for (prio = 0; prio < MAX_PRIO; prio++)
		if (!empty(&mlq_ready_queue[prio])) {
			pthread_mutex_unlock(&queue_lock);
			return 0;
		}
	result = 1;
#else
	result = empty(&ready_queue);
#endif
	pthread_mutex_unlock(&queue_lock);
	return result;
}

void init_scheduler(void) {
#ifdef MLQ_SCHED
	int i;

	for (i = 0; i < MAX_PRIO; i++) {
		mlq_ready_queue[i].size = 0;
		slot[i] = MAX_PRIO - i;
	}
#endif
	ready_queue.size = 0;
	run_queue.size = 0;
	running_list.size = 0;
	pthread_mutex_init(&queue_lock, NULL);

}

void finish_scheduler(void) {
	pthread_mutex_destroy(&queue_lock);
}

void scheduler_lock_queues(void) {
	pthread_mutex_lock(&queue_lock);
}

void scheduler_unlock_queues(void) {
	pthread_mutex_unlock(&queue_lock);
}

void remove_proc_from_running_list(struct pcb_t *proc) {
	if (proc == NULL)
		return;

	pthread_mutex_lock(&queue_lock);
	purgequeue(&running_list, proc);
	pthread_mutex_unlock(&queue_lock);
}

struct pcb_t *find_proc_by_pid(uint32_t pid) {
	struct pcb_t *proc = NULL;

	scheduler_lock_queues();
	for (int i = running_list.size - 1; i >= 0; i--) {
		struct pcb_t *candidate = running_list.proc[i];
		if (candidate != NULL && candidate->pid == pid) {
			proc = candidate;
			break;
		}
	}
	scheduler_unlock_queues();
	return proc;
}

#ifdef MLQ_SCHED
/* 
 *  Stateful design for routine calling
 *  based on the priority and our MLQ policy
 *  We implement stateful here using transition technique
 *  State representation   prio = 0 .. MAX_PRIO, curr_slot = 0..(MAX_PRIO - prio)
 */
struct pcb_t * get_mlq_proc(void) {
	struct pcb_t * proc = NULL;

	pthread_mutex_lock(&queue_lock);
	for (int i = 0; i < MAX_PRIO; i++) {
		if (slot[i] > 0 && !empty(&mlq_ready_queue[i])) {
			proc = dequeue(&mlq_ready_queue[i]);
			if (proc != NULL)
				slot[i]--;
			break;
		}
	}

	if (proc == NULL) {
		int has_ready_proc = 0;
		for (int i = 0; i < MAX_PRIO; i++) {
			if (!empty(&mlq_ready_queue[i])) {
				has_ready_proc = 1;
				break;
			}
		}

		if (has_ready_proc) {
			for (int i = 0; i < MAX_PRIO; i++)
				slot[i] = MAX_PRIO - i;

			for (int i = 0; i < MAX_PRIO; i++) {
				if (!empty(&mlq_ready_queue[i])) {
					proc = dequeue(&mlq_ready_queue[i]);
					if (proc != NULL)
						slot[i]--;
					break;
				}
			}
		}
	}

	if (proc != NULL)
		enqueue(&running_list, proc);
	pthread_mutex_unlock(&queue_lock);
	return proc;
}

int put_mlq_proc(struct pcb_t * proc) {
	if (proc == NULL)
		return 0;
	if (proc->prio >= MAX_PRIO) {
		fprintf(stderr, "Cannot reschedule PID %u: invalid priority %u\n",
			proc->pid, proc->prio);
		return 0;
	}
	if (proc->krnl == NULL) {
		fprintf(stderr, "Cannot reschedule PID %u: missing kernel context\n",
			proc->pid);
		return 0;
	}

	pthread_mutex_lock(&queue_lock);
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->mlq_ready_queue = mlq_ready_queue;
	proc->krnl->running_list = &running_list;
	if (mlq_ready_queue[proc->prio].size >= MAX_QUEUE_SIZE) {
		fprintf(stderr, "Cannot reschedule PID %u: priority queue %u is full\n",
			proc->pid, proc->prio);
		pthread_mutex_unlock(&queue_lock);
		return 0;
	}
	purgequeue(&running_list, proc);
	enqueue(&mlq_ready_queue[proc->prio], proc);
	pthread_mutex_unlock(&queue_lock);
	return 1;
}

enum sched_add_result add_mlq_proc(struct pcb_t * proc) {
	if (proc == NULL)
		return SCHED_ADD_INVALID;
	if (proc->prio >= MAX_PRIO) {
		fprintf(stderr, "Cannot schedule PID %u: invalid priority %u\n",
			proc->pid, proc->prio);
		return SCHED_ADD_INVALID;
	}
	if (proc->krnl == NULL) {
		fprintf(stderr, "Cannot schedule PID %u: missing kernel context\n",
			proc->pid);
		return SCHED_ADD_INVALID;
	}

	pthread_mutex_lock(&queue_lock);
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->mlq_ready_queue = mlq_ready_queue;
	proc->krnl->running_list = &running_list;
	if (mlq_ready_queue[proc->prio].size >= MAX_QUEUE_SIZE) {
		pthread_mutex_unlock(&queue_lock);
		return SCHED_ADD_FULL;
	}
	enqueue(&mlq_ready_queue[proc->prio], proc);
	pthread_mutex_unlock(&queue_lock);
	return SCHED_ADD_OK;
}

struct pcb_t * get_proc(void) {
	return get_mlq_proc();
}

int put_proc(struct pcb_t * proc) {
	return put_mlq_proc(proc);
}

enum sched_add_result add_proc(struct pcb_t * proc) {
	return add_mlq_proc(proc);
}
#else
struct pcb_t * get_proc(void) {
	struct pcb_t * proc = NULL;

	pthread_mutex_lock(&queue_lock);
	if (!empty(&ready_queue))
		proc = dequeue(&ready_queue);
	if (proc != NULL)
		enqueue(&running_list, proc);
	pthread_mutex_unlock(&queue_lock);

	return proc;
}

int put_proc(struct pcb_t * proc) {
	if (proc == NULL || proc->krnl == NULL)
		return 0;

	pthread_mutex_lock(&queue_lock);
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;
	if (ready_queue.size >= MAX_QUEUE_SIZE) {
		fprintf(stderr, "Cannot reschedule PID %u: ready queue is full\n",
			proc->pid);
		pthread_mutex_unlock(&queue_lock);
		return 0;
	}
	purgequeue(&running_list, proc);
	enqueue(&ready_queue, proc);
	pthread_mutex_unlock(&queue_lock);
	return 1;
}

enum sched_add_result add_proc(struct pcb_t * proc) {
	if (proc == NULL || proc->krnl == NULL)
		return SCHED_ADD_INVALID;

	pthread_mutex_lock(&queue_lock);
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;
	if (ready_queue.size >= MAX_QUEUE_SIZE) {
		pthread_mutex_unlock(&queue_lock);
		return SCHED_ADD_FULL;
	}
	enqueue(&ready_queue, proc);
	pthread_mutex_unlock(&queue_lock);
	return SCHED_ADD_OK;
}
#endif


