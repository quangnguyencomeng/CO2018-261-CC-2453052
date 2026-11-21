# Member 1 - Process Queue and MLQ Scheduler

## Assigned Work

Member 1 owns the process queue implementation and the Multi-Level Queue (MLQ) scheduler:

- `src/queue.c`: FIFO queue operations
- `src/sched.c`: priority queues, quotas, running-process lookup, and queue lifecycle
- `src/os.c` integration: loader, CPU dispatch, rescheduling, and termination cleanup
- `tests/sched_test.c`: scheduler unit and regression tests

## MLQ Configuration

The scheduler is enabled by `MLQ_SCHED` and uses `MAX_PRIO = 140`.
Priority 0 has the highest priority. Each priority queue has a time-slice budget:

```text
slot[prio] = MAX_PRIO - prio
```

The scheduler scans priority levels from 0 upward. If every ready queue has exhausted
its quota, it resets all quotas and starts a new scheduling round.

## Queue Implementation

`enqueue()` appends a PCB until `MAX_QUEUE_SIZE` entries are present.
`dequeue()` removes the first PCB and shifts the remaining entries left.
`purgequeue()` removes a PCB by pointer or PID and recompacts the queue.
Both queue and scheduler operations validate NULL pointers and queue bounds.

`dequeue()` is FIFO within each MLQ priority queue because processes at that
level share the same priority. In non-MLQ mode, it selects the PCB with the
smallest numeric `priority` value and preserves FIFO order when priorities tie.

## Running-Process Safety

The scheduler initializes the MLQ ready queues and quotas in `init_scheduler()`.
Queue pointers on a PCB are set by scheduler operations under `queue_lock`.
The scheduler exposes `find_proc_by_pid(pid)` for integration code that needs
to resolve an active process. Syscall code uses that lookup helper and locks the
queues with scheduler helpers before scanning or purging shared queue entries.

## Queue-Full Handling

`put_proc()` reports whether rescheduling succeeded. If the target queue is full,
the CPU keeps the current process and retries dispatch after more time slices.

`add_proc()` returns:

- `SCHED_ADD_OK`: process enqueued
- `SCHED_ADD_FULL`: target queue full
- `SCHED_ADD_INVALID`: invalid process or priority

The loader waits for one time slot and retries when `SCHED_ADD_FULL` is returned,
so a newly loaded process is not silently dropped.

## CPU and Loader Integration

The loader uses `add_proc()` and waits for a time slot when the target queue is full.
The CPU uses `get_proc()` and `put_proc()` for dispatch and rescheduling.
CPU idle/shutdown behavior belongs to the OS integration scope and must be handled
consistently with the loader's `done` state.

## Testing

The scheduler test suite covers:

1. FIFO enqueue, dequeue, and purge
2. Dispatch and running-list lifecycle
3. Rescheduling when the target queue is full
4. Retrying loader enqueue after a queue becomes full
5. MLQ priority quotas
6. PID lookup

On Unix/Linux:

```bash
make test-sched
```

On Windows with MinGW:

```bash
gcc -std=gnu11 -Wall -Wextra -Werror -Iinclude -pthread \
  tests/sched_test.c src/queue.c src/sched.c -o sched_test.exe
./sched_test.exe
```

## Implementation Timeline

| Day | Work |
|---|---|
| 1-2 | Implement FIFO queue operations and queue bounds |
| 3-5 | Implement MLQ dispatch and priority quotas |
| 6 | Integrate running-list cleanup and PID lookup |
| 7 | Add queue-full recovery for reschedule and loader paths |
| 8 | Add full-queue, FIFO, PID, and termination tests |
| 9 | Run scheduler tests and update documentation |

## Gantt Diagram

Generate a Mermaid Gantt diagram from the repository scheduler logs:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\gantt_member1.ps1
```

The command reads `sched.output`, `sched_0.output`, `sched_1.output`, and
`os_1_singleCPU_mlq_paging.output`, then creates `docs/member1-gantt.md`.

## Q3 - What distinguishes MLQ scheduling?

MLQ separates processes into fixed priority queues. A process joins the queue for
its configured priority and is dispatched according to priority order and per-level
time-slice quotas. Higher priority levels receive larger quotas, but quota exhaustion
allows lower levels to run instead of starving them indefinitely.

A completed process leaves the system. A preempted process returns to its original
priority queue. Scheduler and syscall queue operations use `queue_lock` so CPU,
loader, and syscall threads cannot modify shared queue entries concurrently.

## PR Status

PR #1 for `member-1/scheduler_queue` was merged into `main` on November 20, 2026.
Merge commit: `fd886a8f4240ada3c744794da771d58421753103`.
