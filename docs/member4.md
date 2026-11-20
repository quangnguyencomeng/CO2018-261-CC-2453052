# Member 4 memory operations

The code is in `src/libmem.c`.

- `ALLOC` tries the free list first and uses syscall 17 to grow the VMA when needed. If `FREE` releases `[0, 300)` and the next allocation takes 100 bytes from it, `[100, 300)` stays on the free list.
- `KMALLOC` reserves consecutive RAM frames and maps them in the shared kernel page table at `krnl->mm`. Each process keeps its own region IDs.
- `READ` and `WRITE` use the process's user page table. `COPY_FROM_USER` and `COPY_TO_USER` check both regions before copying.
- `pg_getpage()` uses a free RAM frame to load a swapped page when one is available. If RAM is full, it swaps out the oldest resident page in the current process. That path needs a free swap slot.

## Integration

Call `free_pcb_memph()` after the process's CPU, syscall, and DMA references have drained. It returns the process's RAM frames and swap slots and calls `destroy_mm()`. The caller frees the `mm` struct.

## Test status

With Member 2's page tables and stubs for syscall 17 and the memory devices, tests passed for region reuse, copies across a page boundary, invalid access, FIFO with one resident page, and frame cleanup.

A full OS run still needs Member 3's syscall and frame code and Member 5's exit and DMA code.
