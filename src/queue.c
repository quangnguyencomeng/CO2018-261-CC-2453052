#include <stdio.h>
#include <stdlib.h>
#include "queue.h"

int empty(struct queue_t *q)
{
        if (q == NULL)
                return 1;
        return (q->size == 0);
}

void enqueue(struct queue_t *q, struct pcb_t *proc)
{
        if (q == NULL || proc == NULL)
                return;
        if (q->size < 0 || q->size >= MAX_QUEUE_SIZE) {
                fprintf(stderr, "Cannot enqueue PID %u: queue is full or invalid\n",
                        proc->pid);
                return;
        }

        q->proc[q->size++] = proc;
}

struct pcb_t *dequeue(struct queue_t *q)
{
        if (q == NULL || q->size <= 0 || q->size > MAX_QUEUE_SIZE)
                return NULL;

        struct pcb_t *proc = q->proc[0];
        for (int i = 1; i < q->size; i++)
                q->proc[i - 1] = q->proc[i];
        q->proc[--q->size] = NULL;

        return proc;
}

struct pcb_t *purgequeue(struct queue_t *q, struct pcb_t *proc)
{
        if (q == NULL || proc == NULL || q->size <= 0 ||
            q->size > MAX_QUEUE_SIZE)
                return NULL;

        for (int i = 0; i < q->size; i++) {
                struct pcb_t *queued_proc = q->proc[i];
                if (queued_proc == proc ||
                    (queued_proc != NULL && queued_proc->pid == proc->pid)) {
                        for (int j = i + 1; j < q->size; j++)
                                q->proc[j - 1] = q->proc[j];
                        q->proc[--q->size] = NULL;
                        return queued_proc;
                }
        }

        return NULL;
}