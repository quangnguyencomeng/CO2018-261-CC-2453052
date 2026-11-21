# Member 3: Virtual Memory Area, Physical Memory, and System Calls

## 1. System Call Interface and PID Passing

User processes do not pass a `struct pcb_t *` pointer across the syscall boundary. Instead, `libsyscall()` and `libmem.c` place arguments in `struct sc_regs` (`a1`–`a3`) and call `_syscall(krnl, pid, nr, &regs)`. At build time, `src/syscalltbl.sh` converts `src/syscall.tbl` into `src/syscalltbl.lst` to generate the dispatcher in `src/syscall.c`.

Inside kernel handlers (`src/sys_mem.c` and `src/sys_killall.c`), the dummy `malloc` for `caller` was removed. The kernel resolves the calling PCB by matching `pid` in `running_list` via `find_proc_by_pid(pid)` in `src/sched.c` under `queue_lock`, and `__sys_killall()` guards its scans across `running_list` and the ready queues with `scheduler_lock_queues()` / `scheduler_unlock_queues()`.

### Registered System Calls

| Number | Name | File | Behavior |
| :---: | :--- | :--- | :--- |
| `0` | `listsyscall` | `src/sys_listsyscall.c` | Prints all entries in `sys_call_table[]` (`sc1`). |
| `17` | `memmap` | `src/sys_mem.c` | Dispatches `MAP`, `INC`, `SWP`, `IO_READ`, and `IO_WRITE`. Returns `-1` if a process-scoped op has an unknown PID. |
| `101` | `killall` | `src/sys_killall.c` | Reads a target process name from region `regs->a1` via `__read()` (terminated by `-1` or `'\0'`), marks matches in `running_list` as finished, and purges matches from ready queues (`sc2`). |
| `440` | `xxx` | `src/sys_xxxhandler.c` | Prints `The first system call parameter <a1>` (`sc3`, Section 1.6.3). |

Note for Member 2: `SYSMEM_MAP_OP` delegates directly to `vmap_pgd_memset()` and returns its status, which is compatible with the zero-slot reservation implemented in `src/mm64.c`.

---

## 2. Physical Memory and VM Area Management

### Physical Memory (`src/mm-memphy.c`)

- `MEMPHY_get_freefp()` pops the head of `free_fp_list`, returns `fp->fpn`, advances the list pointer, and frees the node.
- `MEMPHY_dump()` prints non-zero bytes in `mp->storage` as `BYTE %08x: %d`.
- Fixed starter initialization bugs: `MEMPHY_format()` now sets `fst->fp_next = NULL` (fixing 1-frame RAM in `small_4K`), and `init_memphy()` resets `free_fp_list`/`used_fp_list` to `NULL` and zeroes `storage` with `calloc()` (fixing 0-frame `small_1K` and unused swap devices).
- Added bounds checking (`addr < mp->maxsz`), fixed the inverted `rdmflg` condition in sequential read/write, and protected `MEMPHY_*` routines with `memphy_lock`.

### Virtual Memory Area (`src/mm-vm.c`)

- `get_vma_by_num()` checks for `NULL` safely instead of dereferencing past the end of the VMA list.
- `get_vm_area_node_at_brk()` returns `[cur_vma->sbrk, cur_vma->sbrk + alignedsz)` from `caller->mm`.
- `validate_overlap_vm_area()` checks the planned range `[vmastart, vmaend)` against all other VMAs (fixing the starter check that compared the old `cur_area` range).
- `inc_vma_limit()` aligns `inc_sz` to 4 KB (`PAGING64_PAGE_ALIGNSZ`), checks overlap, and calls `vm_map_ram()` before updating `vm_end` and `sbrk`. If RAM runs out (`-3000`), `vm_end` and `sbrk` remain unchanged. Otherwise both advance by `inc_amt`, and any unused tail `[old_sbrk + inc_sz, cur_vma->vm_end)` is added to `vm_freerg_list`.

### Data Segment State Example (`m0s` / `m2s`)

| Step | Instruction | `[vm_start, vm_end)` / `sbrk` | Allocated (`symrgtbl`) | Free List (`vm_freerg_list`) | RAM Frames |
| :---: | :--- | :--- | :--- | :--- | :---: |
| 0 | `init_mm()` | `[0, 0)` / `0` | none | empty | 0 |
| 1 | `alloc 300 0` | `[0, 4096)` / `4096` | `rg[0] = [0, 300)` | `[300, 4096)` | 1 |
| 2 | `alloc 100 1` | `[0, 4096)` / `4096` | `rg[0] = [0, 300)`, `rg[1] = [300, 400)` | `[400, 4096)` | 1 |
| 3 | `free 0` | `[0, 4096)` / `4096` | `rg[1] = [300, 400)` | `[0, 300) -> [400, 4096)` | 1 |
| 4 | `alloc 100 2` | `[0, 4096)` / `4096` | `rg[1] = [300, 400)`, `rg[2] = [0, 100)` | `[100, 300) -> [400, 4096)` | 1 |

---

## 3. Q1: Unified System Call Interface (Page 9)

**OS Design:** Putting files, memory, and I/O devices behind one interface (like UNIX `read`/`write`/`close` or `memmap` in this simulator) keeps the kernel entry layer small. The kernel routes calls through a dispatch table or `switch` statement to subsystem handlers, so new devices can be added without adding new syscalls. Permission checks and auditing also happen in one place.

**Performance Trade-offs:** A generic byte-stream interface adds trap and dispatch overhead and typically requires copying between kernel and user buffers. It cannot express hardware-specific paths such as zero-copy transfers, memory-mapped I/O, or asynchronous DMA, which is why operating systems still expose separate calls like `mmap`, `ioctl`, and `io_uring`.

**Error Handling:** Different subsystems fail for very different reasons (disk errors, dropped connections, page-allocation failures, device timeouts), yet all map onto a small set of shared codes (`-1`, `EIO`, `EINVAL`, `EAGAIN`). Callers lose low-level failure details and still have to handle behavioral differences such as partial reads on sockets/pipes versus regular files.

**Portability vs. Efficiency:** Unified calls make user programs portable and easy to compose (for example, shell redirection and pipelines). In practice, operating systems keep the unified interface as the default for general workloads and provide narrow specialized interfaces where throughput or latency matters.

---

## 4. Q2: Detecting and Handling Long-Running System Calls (Page 11)

### Detection

- **Timer interrupts:** Hardware timers keep firing even in kernel mode. When a process exceeds its time slice, the timer handler flags it for rescheduling (e.g. `TIF_NEED_RESCHED`).
- **Kernel watchdogs:** A soft-lockup watchdog detects a CPU stuck in kernel mode without scheduling other tasks; a hard-lockup watchdog uses Non-Maskable Interrupts (NMIs) to catch hangs even when interrupts are disabled; a hung-task check flags threads stuck in uninterruptible sleep.
- **Timeouts and signals:** Device drivers set timers for pending I/O so stalled hardware triggers a timeout, and user space can send signals (`SIGINT`, `SIGALRM`, `SIGKILL`) to cancel a slow call.

### Handling

- **Blocking on I/O:** If a syscall waits for slow disk, network, or DMA activity, the kernel moves the process to a wait queue (`WAITING`/`BLOCKED`) and schedules another process. An interrupt (IRQ / `dma_done_callback`) wakes it when the I/O finishes.
- **Kernel preemption:** A preemptible kernel can suspend a CPU-heavy syscall at a safe point (when no spinlock is held), run a higher-priority task, and resume the syscall later.
- **Clean abort and rollback:** When an interruptible wait hits a timeout or signal, the kernel unwinds cleanly, releases any locks and temporary buffers, and returns `-ETIMEDOUT`, `-EINTR`, or `-ERESTARTSYS`.
- **Offloading and recovery:** Heavy copies can be handed off to DMA co-processors or worker threads so the syscall returns quickly. If a kernel bug causes an unrecoverable hang, the watchdog logs a kernel oops/stack trace and terminates the context or triggers a kernel panic to protect system state.

---

## 5. Verification and Integration Notes

### Running Tests

On Ubuntu/Linux:

```bash
make test-member3
make test-sched
```

On Windows (MinGW GCC, after generating `src/syscalltbl.lst`):

```powershell
gcc -std=gnu11 -Wall -Wextra -g -pthread -Iinclude -Isrc tests/member3_test.c src/queue.c src/sched.c src/mm64.c src/mm-memphy.c src/mm-vm.c src/sys_mem.c src/sys_killall.c src/sys_xxxhandler.c src/sys_listsyscall.c src/syscall.c src/libstd.c -o obj/member3_test.exe
.\obj\member3_test.exe
```

### Results

- `tests/member3_test.c` passed all 4 suites against Member 1's `queue.c`/`sched.c` and Member 2's `mm64.c`: `MEMPHY` edge cases (`0B`, `2KB`, `4KB`, sequential I/O, dump), VMA overlap and OOM rollback, syscalls `0`/`17`/`101`/`440`, and 4-thread `MEMPHY` concurrency.
- `tests/sched_test.c` (Member 1) and `tests/mm64_test.c` (Member 2) both passed.
- A local 4-member build with `member4-libmem` and stubbed DMA ran `os_syscall_list`, `os_syscall`, `os_sc`, `os_0_mlq_paging`, `os_1_singleCPU_mlq_paging`, `os_1_mlq_paging`, `os_1_mlq_paging_small_1K`, `os_1_mlq_paging_small_4K`, and `os_2_singleCPU_mlq_paging` with exit code `0`.

### Phase 3 Synchronization Notes

- `mm-memphy.c` guards `MEMPHY_*` calls with `memphy_lock`, while Member 2's `return_frames()` and Member 4's `__kmalloc()` edit `ram->free_fp_list` directly under other locks. Unifying frame-list locking (e.g. a mutex on `struct memphy_struct` or routing returns through `MEMPHY_put_freefp()`) will remove that race.
- Scheduler queue synchronization for syscalls is resolved: `src/sched.c` exposes `find_proc_by_pid()`, `scheduler_lock_queues()`, and `scheduler_unlock_queues()`, which `src/sys_mem.c` and `src/sys_killall.c` now use under `queue_lock`.
