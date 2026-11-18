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

	init_scheduler();
	assert(queue_empty());
	add_proc(&first);
	add_proc(&second);

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

static void test_mlq_quota(void)
{
	struct pcb_t high, low;
	struct krnl_t kernel;

	memset(&kernel, 0, sizeof(kernel));
	init_proc(&high, &kernel, 1, 0);
	init_proc(&low, &kernel, 2, MAX_PRIO - 1);

	init_scheduler();
	add_proc(&high);
	add_proc(&low);

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
	test_mlq_quota();
	return 0;
}
