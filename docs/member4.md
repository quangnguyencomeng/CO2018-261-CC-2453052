# Member 4: User and Kernel Memory

## Allocation and access

`ALLOC` first searches the process's list of free regions. If no region is large enough, it requests a VMA extension through system call 17. The library records the allocation in the symbol table only when the call succeeds and both `sbrk` and `vm_end` cover the requested range. `FREE` returns a user region to the free list. For example, allocating 100 bytes from a freed region `[0, 300)` allocates `[0, 100)` and leaves `[100, 300)` available.

Each process has its own region symbol table. User regions have `mode_bit = 1` and use the process's page table. Kernel regions have `mode_bit = 0` and use the shared page table at `krnl->mm`. `KMALLOC` finds a run of contiguous RAM frames, maps it in the upper half of the virtual address space, and zeroes the frames before publishing the allocation. Two processes can use the same region ID for separate allocations.

`READ` and `WRITE` accept user regions. `COPY_FROM_USER` and `COPY_TO_USER` check the direction and the full range of both regions before copying any bytes. The physical address is `fpn * 4096 + page_offset`. Byte reads and writes go through system call 17, which identifies the process by PID.

## Page replacement and cleanup

On a fault for a swapped-out page, the library first looks for a free RAM frame. If RAM is full, FIFO selects the page that has been resident the longest in the current process. The library writes the victim to the active swap device, loads the requested page into RAM, updates both PTEs, and moves the newly resident page to the head of the FIFO list. When no RAM frame is free, this policy requires a free swap slot and a resident page in the faulting process.

`free_pcb_memph()` reclaims the process's kernel allocations, walks the leaf PTEs to return RAM frames and swap slots, and calls `destroy_mm()`. The caller frees the `mm` struct after CPU, system call, and DMA references have drained. Member 5 should use `krnl->mm->pgd` as the shared kernel page table root.

## Q4. Why combine segmentation with paging?

Segmentation organizes memory into logical regions such as code, heap, and stack. The OS can apply bounds, permissions, and sharing policies to each region. Placing variable-sized segments in contiguous physical memory can cause external fragmentation. Paging lets the pages of a segment occupy any available frames while preserving a contiguous virtual range. The combination retains region-based protection and makes physical frame allocation more flexible. It also adds metadata, address translation overhead, and possible unused space in the last page of each segment.

## Q6. What are the trade-offs between paging and contiguous allocation?

Paging uses fixed-size frames, can use scattered free frames, and supports swapping one page at a time. Its costs include page tables, address translation, and internal fragmentation. Contiguous allocation simplifies address calculations and suits DMA transfers that require a single physical range. External fragmentation can cause an allocation to fail even when total free memory is sufficient. In this simulator, user regions use paging and `KMALLOC` requires consecutive physical frames.

## Verification

The MinGW GCC tests passed with the five-level page tables from `member2-paging` and test doubles for system calls, RAM, and swap. The tests covered user-region reuse, bounds and permissions, copies across page boundaries in both directions, contiguous kernel allocation under fragmentation, FIFO replacement with one resident page, and reclamation of RAM frames and swap slots. Member 2's standalone paging tests also passed.

Full OS and Verilator validation depends on Member 3's system call and frame management code and Member 5's process lifecycle and DMA integration. The provided workloads include `os_1_singleCPU_mlq_paging`, `os_1_mlq_paging_small_1K`, `os_1_mlq_paging_small_4K`, and `os_2_singleCPU_mlq_paging`.
