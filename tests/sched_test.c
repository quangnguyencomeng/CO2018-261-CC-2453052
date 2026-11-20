#include "queue.h"
#include "sched.h"

#include <assert.h>
#include <string.h>

static void init_proc(struct pcb_t *proc, struct krnl_t *kernel,
		      uint32_t pid, uint32_t prio)
{
	memset(proc, 0, sizeof(*proc));
	proc->pid = pid;
	proc->prio = prio;
	proc->krnl = kernel;
}

static void test_queue_fifo_and_purge(void)
{
	struct pcb_t first, second, second_copy;
	struct queue_t queue = {0};

	init_proc(&first, NULL, 1, 0);
	init_proc(&second, NULL, 2, 0);
	init_proc(&second_copy, NULL, 2, 0);
	enqueue(&queue, &first);
	enqueue(&queue, &second);

	assert(dequeue(&queue) == &first);
	assert(purgequeue(&queue, &second_copy) == &second);
	assert(dequeue(&queue) == NULL);
}

static void test_mlq_and_running_list(void)
{
	struct pcb_t first, second;
	struct krnl_t kernel;

	memset(&kernel, 0, sizeof(kernel));
	init_proc(&first, &kernel, 1, 0);
	init_proc(&second, &kernel, 2, 0);

	init_scheduler(&kernel);
	assert(kernel.ready_queue != NULL);
	assert(kernel.running_list != NULL);
	assert(kernel.mlq_ready_queue != NULL);
	assert(queue_empty());
	assert(add_proc(&first) == SCHED_ADD_OK);
	assert(add_proc(&second) == SCHED_ADD_OK);

	assert(get_proc() == &first);
	assert(kernel.running_list->size == 1);
	put_proc(&first);
	assert(kernel.running_list->size == 0);
	assert(get_proc() == &second);
	assert(get_proc() == &first);
	assert(kernel.running_list->size == 2);
	remove_proc_from_running_list(&first);
	remove_proc_from_running_list(&second);
	assert(kernel.running_list->size == 0);
	assert(queue_empty());
	finish_scheduler();
}

static void test_full_queue_preserves_running_process(void)
{
	struct pcb_t procs[MAX_QUEUE_SIZE + 1];
	struct krnl_t kernel;

	memset(&kernel, 0, sizeof(kernel));
	for (int i = 0; i <= MAX_QUEUE_SIZE; i++)
		init_proc(&procs[i], &kernel, (uint32_t)(i + 1), 0);

	init_scheduler(&kernel);
	for (int i = 0; i < MAX_QUEUE_SIZE; i++)
		add_proc(&procs[i]);

	assert(get_proc() == &procs[0]);
	add_proc(&procs[MAX_QUEUE_SIZE]);

	assert(!put_proc(&procs[0]));
	assert(kernel.running_list->size == 1);
	assert(get_proc() == &procs[1]);
	remove_proc_from_running_list(&procs[1]);
	assert(put_proc(&procs[0]));
	assert(kernel.running_list->size == 0);

	for (int i = 2; i < MAX_QUEUE_SIZE; i++) {
		assert(get_proc() == &procs[i]);
		remove_proc_from_running_list(&procs[i]);
	}
	assert(get_proc() == &procs[MAX_QUEUE_SIZE]);
	remove_proc_from_running_list(&procs[MAX_QUEUE_SIZE]);
	assert(get_proc() == &procs[0]);
	remove_proc_from_running_list(&procs[0]);
	assert(kernel.running_list->size == 0);
	assert(queue_empty());
	finish_scheduler();
}

static void test_add_process_retries_after_full_queue(void)
{
	struct pcb_t procs[MAX_QUEUE_SIZE + 1];
	struct krnl_t kernel;

	memset(&kernel, 0, sizeof(kernel));
	for (int i = 0; i <= MAX_QUEUE_SIZE; i++)
		init_proc(&procs[i], &kernel, (uint32_t)(i + 1), 0);

	init_scheduler(&kernel);
	for (int i = 0; i < MAX_QUEUE_SIZE; i++)
		assert(add_proc(&procs[i]) == SCHED_ADD_OK);
	assert(add_proc(&procs[MAX_QUEUE_SIZE]) == SCHED_ADD_FULL);

	assert(get_proc() == &procs[0]);
	remove_proc_from_running_list(&procs[0]);
	assert(add_proc(&procs[MAX_QUEUE_SIZE]) == SCHED_ADD_OK);

	for (int i = 1; i < MAX_QUEUE_SIZE; i++) {
		assert(get_proc() == &procs[i]);
		remove_proc_from_running_list(&procs[i]);
	}
	assert(get_proc() == &procs[MAX_QUEUE_SIZE]);
	remove_proc_from_running_list(&procs[MAX_QUEUE_SIZE]);
	assert(queue_empty());
	finish_scheduler();
}

static void test_mlq_quota(void)
{
	struct pcb_t high, low;
	struct krnl_t kernel;

	memset(&kernel, 0, sizeof(kernel));
	init_proc(&high, &kernel, 1, 0);
	init_proc(&low, &kernel, 2, MAX_PRIO - 1);

	init_scheduler(&kernel);
	assert(add_proc(&high) == SCHED_ADD_OK);
	assert(add_proc(&low) == SCHED_ADD_OK);

	for (int i = 0; i < MAX_PRIO; i++) {
		assert(get_proc() == &high);
		put_proc(&high);
	}

	assert(get_proc() == &low);
	assert(kernel.running_list->size == 1);
	remove_proc_from_running_list(&low);
	assert(get_proc() == &high);
	assert(kernel.running_list->size == 1);
	remove_proc_from_running_list(&high);
	assert(queue_empty());
	finish_scheduler();
}

int main(void)
{
	test_queue_fifo_and_purge();
	test_mlq_and_running_list();
	test_full_queue_preserves_running_process();
	test_add_process_retries_after_full_queue();
	test_mlq_quota();
	return 0;
}
