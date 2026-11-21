/*
 * Regression tests for allocation-free frame rollback between src/mm64.c and
 * real src/mm-memphy.c under both calloc() and malloc() fault injection.
 */

#include "mm64.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

void *__real_calloc(size_t count, size_t size);
void *__real_malloc(size_t size);

static int fail_calloc_after = -1;
static int fail_malloc_after = -1;

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_calloc_after == 0)
        return NULL;
    if (fail_calloc_after > 0)
        --fail_calloc_after;
    return __real_calloc(count, size);
}

void *__wrap_malloc(size_t size)
{
    if (fail_malloc_after == 0)
        return NULL;
    if (fail_malloc_after > 0)
        --fail_malloc_after;
    return __real_malloc(size);
}

static void cleanup_memphy(struct memphy_struct *mp)
{
    while (mp->free_fp_list != NULL) {
        struct framephy_struct *fp = mp->free_fp_list;
        mp->free_fp_list = fp->fp_next;
        free(fp);
    }
    free(mp->storage);
    mp->storage = NULL;
    mp->maxsz = 0;
}

static uint64_t collect_free_fpn_mask(const struct memphy_struct *ram, int expected_count)
{
    int count = 0;
    uint64_t mask = 0;

    for (struct framephy_struct *fp = ram->free_fp_list; fp != NULL; fp = fp->fp_next) {
        assert(fp->owner == NULL);
        assert(fp->fpn < 64);
        assert((mask & (UINT64_C(1) << fp->fpn)) == 0);
        mask |= UINT64_C(1) << fp->fpn;
        ++count;
    }

    assert(count == expected_count);
    return mask;
}

static void test_alloc_pages_range_dual_oom_rollback(void)
{
    struct memphy_struct ram;
    struct mm_struct mm;
    struct krnl_t kernel = {.mram = &ram};
    struct pcb_t proc = {.pid = 1, .mm = &mm, .krnl = &kernel};
    struct framephy_struct *frames = (struct framephy_struct *)0x1;

    assert(init_memphy(&ram, 2 * PAGING64_PAGESZ, 1) == 0);
    assert(init_mm(&mm, &proc) == 0);

    const uint64_t before_mask = collect_free_fpn_mask(&ram, 2);
    assert(before_mask == UINT64_C(0x3));

    /*
     * 1st calloc() reserves node for frame 0; 2nd calloc() fails;
     * every malloc() also fails during rollback.
     */
    fail_calloc_after = 1;
    fail_malloc_after = 0;
    addr_t res = alloc_pages_range(&proc, 2, &frames);
    fail_calloc_after = -1;
    fail_malloc_after = -1;

    assert(res == (addr_t)-1);
    assert(frames == NULL);
    assert(collect_free_fpn_mask(&ram, 2) == before_mask);
    assert(mm.fifo_pgn == NULL);
    assert(mm.page_table_bytes == 4096);
    assert(pte_get_entry(&mm, 0, &proc) == 0);
    assert(pte_get_entry(&mm, 1, &proc) == 0);

    /* OOM on RAM frames (request 3 when only 2 exist) with malloc() blocked */
    fail_malloc_after = 0;
    res = alloc_pages_range(&proc, 3, &frames);
    fail_malloc_after = -1;

    assert(res == (addr_t)-3000);
    assert(frames == NULL);
    assert(collect_free_fpn_mask(&ram, 2) == before_mask);

    /* Verify API edge cases */
    assert(MEMPHY_put_freefp_list(NULL, NULL) == -1);
    assert(MEMPHY_put_freefp_list(&ram, NULL) == 0);
    assert(collect_free_fpn_mask(&ram, 2) == before_mask);

    destroy_mm(&mm);
    cleanup_memphy(&ram);
}

static void test_vm_map_ram_dual_oom_rollback(void)
{
    struct memphy_struct ram;
    struct mm_struct mm;
    struct krnl_t kernel = {.mram = &ram};
    struct pcb_t proc = {.pid = 2, .mm = &mm, .krnl = &kernel};
    struct vm_rg_struct region = {.rg_start = 123, .rg_end = 456};

    assert(init_memphy(&ram, 2 * PAGING64_PAGESZ, 1) == 0);
    assert(init_mm(&mm, &proc) == 0);

    const uint64_t before_mask = collect_free_fpn_mask(&ram, 2);
    assert(before_mask == UINT64_C(0x3));

    /*
     * Case 1: alloc_pages_range() succeeds (2 calloc calls -> takes both frames),
     * vmap_page_range() allocates mappings[] (calloc #3) and P4D (calloc #4),
     * then fails on PUD (calloc #5) while malloc() is completely disabled.
     */
    fail_calloc_after = 4;
    fail_malloc_after = 0;
    addr_t res = vm_map_ram(&proc, 0, 2 * PAGING64_PAGESZ, 0, 2, &region);
    fail_calloc_after = -1;
    fail_malloc_after = -1;

    assert(res == (addr_t)-1);
    assert(region.rg_start == 123 && region.rg_end == 456);
    assert(collect_free_fpn_mask(&ram, 2) == before_mask);
    assert(mm.page_table_bytes == 4096);
    assert(mm.fifo_pgn == NULL);
    assert(pte_get_entry(&mm, 0, &proc) == 0);
    assert(pte_get_entry(&mm, 1, &proc) == 0);

    /*
     * Case 2: alloc_pages_range() takes both frames (calloc #1..#2),
     * vmap_page_range() allocates mappings[] (#3) + 4 page tables (#4..#7)
     * and the 1st FIFO node (malloc #1), then fails on the 2nd FIFO node
     * (malloc #2) while further calloc() calls are also disabled.
     */
    fail_calloc_after = 7;
    fail_malloc_after = 1;
    res = vm_map_ram(&proc, 0, 2 * PAGING64_PAGESZ, 0, 2, &region);
    fail_calloc_after = -1;
    fail_malloc_after = -1;

    assert(res == (addr_t)-1);
    assert(region.rg_start == 123 && region.rg_end == 456);
    assert(collect_free_fpn_mask(&ram, 2) == before_mask);
    assert(mm.page_table_bytes == 4096);
    assert(mm.fifo_pgn == NULL);
    assert(pte_get_entry(&mm, 0, &proc) == 0);
    assert(pte_get_entry(&mm, 1, &proc) == 0);

    /*
     * Case 3: Subsequent mapping succeeds cleanly using the preserved {0, 1}
     * frames with no loss or duplication.
     */
    assert(vm_map_ram(&proc, 0, 2 * PAGING64_PAGESZ, 0, 2, &region) == 0);
    assert(region.rg_start == 0 && region.rg_end == 2 * PAGING64_PAGESZ);
    assert(collect_free_fpn_mask(&ram, 0) == 0);

    addr_t pte0 = pte_get_entry(&mm, 0, &proc);
    addr_t pte1 = pte_get_entry(&mm, 1, &proc);
    assert(PAGING_PAGE_PRESENT(pte0) && PAGING_PAGE_PRESENT(pte1));
    uint64_t mapped_mask = (UINT64_C(1) << PAGING_FPN(pte0)) |
                           (UINT64_C(1) << PAGING_FPN(pte1));
    assert(mapped_mask == before_mask);
    assert(mm.fifo_pgn != NULL && mm.fifo_pgn->pgn == 1);
    assert(mm.fifo_pgn->pg_next != NULL && mm.fifo_pgn->pg_next->pgn == 0);
    assert(mm.fifo_pgn->pg_next->pg_next == NULL);

    destroy_mm(&mm);
    cleanup_memphy(&ram);
}

static void test_vm_map_ram_overlap_mapping_rollback(void)
{
    struct memphy_struct ram;
    struct mm_struct mm;
    struct krnl_t kernel = {.mram = &ram};
    struct pcb_t proc = {.pid = 3, .mm = &mm, .krnl = &kernel};
    struct vm_rg_struct region = {.rg_start = 0, .rg_end = 0};

    assert(init_memphy(&ram, 3 * PAGING64_PAGESZ, 1) == 0);
    assert(init_mm(&mm, &proc) == 0);

    /* Map page 1 first, consuming 1 of the 3 frames. */
    assert(vm_map_ram(&proc, PAGING64_PAGESZ, 2 * PAGING64_PAGESZ,
                      PAGING64_PAGESZ, 1, &region) == 0);
    const addr_t existing_pte1 = pte_get_entry(&mm, 1, &proc);
    const uint64_t remaining_mask = collect_free_fpn_mask(&ram, 2);
    const uint64_t expected_all = remaining_mask | (UINT64_C(1) << PAGING_FPN(existing_pte1));
    assert(expected_all == UINT64_C(0x7));

    /*
     * Attempt to map [0, 8192) which collides with already-mapped page 1.
     * Both remaining frames are reserved, then vmap_page_range() fails on page 1
     * while malloc() fails during rollback.
     */
    region.rg_start = 999;
    region.rg_end = 1000;
    fail_malloc_after = 1; /* allow 1 FIFO node for page 0, fail any malloc in rollback */
    addr_t res = vm_map_ram(&proc, 0, 2 * PAGING64_PAGESZ, 0, 2, &region);
    fail_malloc_after = -1;

    assert(res == (addr_t)-1);
    assert(region.rg_start == 999 && region.rg_end == 1000);
    assert(collect_free_fpn_mask(&ram, 2) == remaining_mask);
    assert(pte_get_entry(&mm, 0, &proc) == 0);
    assert(pte_get_entry(&mm, 1, &proc) == existing_pte1);
    assert(mm.fifo_pgn != NULL && mm.fifo_pgn->pgn == 1 && mm.fifo_pgn->pg_next == NULL);

    destroy_mm(&mm);
    cleanup_memphy(&ram);
}

static struct memphy_struct g_mt_ram;
static struct krnl_t g_mt_krnl;

static void *mt_rollback_worker(void *arg)
{
    int id = *(int *)arg;
    struct mm_struct mm;
    struct pcb_t proc = {.pid = (uint32_t)(id + 1), .mm = &mm, .krnl = &g_mt_krnl};
    assert(init_mm(&mm, &proc) == 0);

    for (int i = 0; i < 400; ++i) {
        struct framephy_struct *frames = NULL;
        assert(alloc_pages_range(&proc, 3, &frames) == (addr_t)-3000 && frames == NULL);
        if (alloc_pages_range(&proc, 1, &frames) == 0 && frames != NULL) {
            assert(MEMPHY_put_freefp_list(&g_mt_ram, frames) == 0);
        }
    }

    destroy_mm(&mm);
    return NULL;
}

static void test_concurrent_memphy_rollback(void)
{
    assert(init_memphy(&g_mt_ram, 2 * PAGING64_PAGESZ, 1) == 0);
    g_mt_krnl.mram = &g_mt_ram;
    const uint64_t before_mask = collect_free_fpn_mask(&g_mt_ram, 2);

    pthread_t threads[4];
    int ids[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; ++i)
        assert(pthread_create(&threads[i], NULL, mt_rollback_worker, &ids[i]) == 0);
    for (int i = 0; i < 4; ++i)
        assert(pthread_join(threads[i], NULL) == 0);

    assert(collect_free_fpn_mask(&g_mt_ram, 2) == before_mask);
    cleanup_memphy(&g_mt_ram);
}

int main(void)
{
    test_alloc_pages_range_dual_oom_rollback();
    test_vm_map_ram_dual_oom_rollback();
    test_vm_map_ram_overlap_mapping_rollback();
    test_concurrent_memphy_rollback();
    puts("PASS: real MEMPHY allocation-free frame rollback under calloc/malloc OOM and concurrency");
    return 0;
}
