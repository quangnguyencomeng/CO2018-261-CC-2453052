#include "os-mm.h"
#include "mm64.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

_Static_assert(sizeof(addr_t) == 8, "os-mm.h must configure MM64 by itself");

void *__real_calloc(size_t count, size_t size);
static int fail_calloc_after = -1;
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_calloc_after == 0)
        return NULL;
    if (fail_calloc_after > 0)
        --fail_calloc_after;
    return __real_calloc(count, size);
}

/* Minimal MEMPHY fixture: only the frame allocator contract is simulated. */
int MEMPHY_get_freefp(struct memphy_struct *ram, addr_t *fpn)
{
    struct framephy_struct *node = ram->free_fp_list;
    if (node == NULL)
        return -1;
    *fpn = node->fpn;
    ram->free_fp_list = node->fp_next;
    free(node);
    return 0;
}

int MEMPHY_put_freefp(struct memphy_struct *ram, addr_t fpn)
{
    if (ram == NULL)
        return -1;
    struct framephy_struct *node = malloc(sizeof(*node));
    if (node == NULL)
        return -1;
    node->fpn = fpn;
    node->owner = NULL;
    node->fp_next = ram->free_fp_list;
    ram->free_fp_list = node;
    return 0;
}

int MEMPHY_read(struct memphy_struct *ram, addr_t address, BYTE *value)
{
    (void)ram; (void)address; (void)value;
    return -1;
}

int MEMPHY_write(struct memphy_struct *ram, addr_t address, BYTE value)
{
    (void)ram; (void)address; (void)value;
    return -1;
}

static int frame_count(struct memphy_struct *ram)
{
    int count = 0;
    unsigned int seen = 0;
    for (struct framephy_struct *p = ram->free_fp_list; p != NULL; p = p->fp_next) {
        assert(p->fpn < 2 && !(seen & (1U << p->fpn)));
        seen |= 1U << p->fpn;
        ++count;
    }
    return count;
}

static void test_frame_allocation(void)
{
    struct mm_struct mm;
    struct memphy_struct ram = {0};
    struct krnl_t kernel = {.mram = &ram};
    struct pcb_t process = {.mm = &mm, .krnl = &kernel};
    struct vm_rg_struct region = {.rg_start = 123, .rg_end = 456};
    struct framephy_struct *frames = NULL;
    assert(init_mm(&mm, &process) == 0);
    for (addr_t i = 0; i < 2; ++i) {
        struct framephy_struct *node = calloc(1, sizeof(*node));
        assert(node != NULL);
        node->fpn = i;
        node->fp_next = ram.free_fp_list;
        ram.free_fp_list = node;
    }
    assert(alloc_pages_range(&process, 3, &frames) == (addr_t)-3000);
    assert(frames == NULL && frame_count(&ram) == 2);
    fail_calloc_after = 1;
    assert(alloc_pages_range(&process, 2, &frames) == (addr_t)-1);
    fail_calloc_after = -1;
    assert(frames == NULL && frame_count(&ram) == 2);
    /* Mapping failure occurs after successful physical reservations. */
    fail_calloc_after = 3;
    assert(vm_map_ram(&process, 0, 8192, 0, 2, &region) == (addr_t)-1);
    fail_calloc_after = -1;
    assert(frame_count(&ram) == 2 && mm.page_table_bytes == 4096);
    assert(region.rg_start == 123 && region.rg_end == 456);
    assert(vm_map_ram(&process, 0, 8192, 1, 1, &region) == (addr_t)-1);
    assert(frame_count(&ram) == 2);
    assert(vm_map_ram(&process, 0, 8192, 0, 2, &region) == 0);
    assert(frame_count(&ram) == 0 && region.rg_start == 0 && region.rg_end == 8192);
    addr_t first = pte_get_entry(&mm, 0, &process);
    addr_t second = pte_get_entry(&mm, 1, &process);
    assert(PAGING_PAGE_PRESENT(first) && PAGING_PAGE_PRESENT(second));
    assert(PAGING_FPN(first) != PAGING_FPN(second));
    assert(vmap_pgd_memset(&process, 0, 2) == -1);
    assert(pte_get_entry(&mm, 0, &process) == first);
    assert(vmap_pgd_memset(&process, 8192, 1) == 0);
    assert(pte_get_entry(&mm, 2, &process) == 0);
    assert(alloc_pages_range(&process, 1, &frames) == (addr_t)-3000);
    assert(alloc_pages_range(&process, -1, &frames) == (addr_t)-1);
    assert(alloc_pages_range(&process, 0, &frames) == 0 && frames == NULL);
    assert(__swap_cp_page(NULL, 0, &ram, 0) == -1);
    assert(__swap_cp_page(&ram, 0, &ram, 1) == -1);
    assert(enlist_vm_rg_node(NULL, NULL) == -1);
    assert(enlist_pgn_node(NULL, 0) == -1);
    destroy_mm(&mm);
}

struct thread_args { struct mm_struct *mm; addr_t first; };
static void *write_pages(void *arg)
{
    const struct thread_args *args = arg;
    for (addr_t i = 0; i < 64; ++i) {
        const addr_t pgn = args->first + i;
        assert(pte_set_fpn(args->mm, pgn, i, NULL) == 0);
        const addr_t pte = pte_get_entry(args->mm, pgn, NULL);
        assert(PAGING_PAGE_PRESENT(pte) && PAGING_FPN(pte) == i);
    }
    return NULL;
}

int main(void)
{
    test_frame_allocation();
    assert(INCLUDE(0, 20, 0, 20));
    assert(INCLUDE(0, 20, 20, 20));
    assert(!INCLUDE(0, 20, 10, 9));
    assert(OVERLAP(0, 8, 4, 12));
    assert(!OVERLAP(0, 8, 8, 12));
    assert(!OVERLAP(0, 8, 3, 3));
    assert(PAGING64_ADDR_OFFST(UINT64_C(0x1234fff)) == 4095);

    addr_t pgd, p4d, pud, pmd, pt;
    assert(get_pd_from_address(0x1fffff, &pgd, &p4d, &pud, &pmd, &pt) == 0);
    assert(pgd == 0 && p4d == 0 && pud == 0 && pmd == 0 && pt == 511);
    assert(get_pd_from_address(0x200000, &pgd, &p4d, &pud, &pmd, &pt) == 0);
    assert(pgd == 0 && p4d == 0 && pud == 0 && pmd == 1 && pt == 0);
    assert(get_pd_from_address(UINT64_C(0x0100000000000000), &pgd, &p4d, &pud, &pmd, &pt) == -1);
    const addr_t high = UINT64_C(0xff00000000000000);
    assert(get_pd_from_address(high, &pgd, &p4d, &pud, &pmd, &pt) == 0);
    assert(pgd == 256 && p4d == 0 && pud == 0 && pmd == 0 && pt == 0);

    struct mm_struct a, b;
    struct pcb_t owner = {.mm = &a};
    assert(init_mm(&a, &owner) == 0);
    assert(init_mm(&b, NULL) == 0);
    assert(a.mmap->vm_mm == &a && a.mmap->vm_next == NULL);
    assert(a.mmap->vm_freerg_list == NULL && a.symrgtbl[0].rg_end == 0);
    assert(a.page_table_bytes == 4096 && a.pte_lookup_reads == 0);
    assert(pte_get_entry(&a, 42, &owner) == 0);
    assert(a.page_table_bytes == 4096 && a.pte_lookup_reads == 1);
    assert(pte_set_entry(&a, 42, 0, &owner) == 0 && a.page_table_bytes == 4096);

    fail_calloc_after = 2;
    assert(pte_set_fpn(&a, 42, 1, &owner) == -1);
    fail_calloc_after = -1;
    assert(a.page_table_bytes == 4096 && a.pgd[0] == 0);

    assert(pte_set_fpn(&a, 42, 0, &owner) == 0);
    assert(a.page_table_bytes == 5 * 4096);
    assert(pte_set_fpn(&b, 42, 7, &owner) == 0);
    addr_t entry = pte_get_entry(&a, 42, &owner);
    assert(PAGING_PAGE_PRESENT(entry) && PAGING_FPN(entry) == 0);
    assert(a.pte_lookup_reads == 6);
    assert(PAGING_FPN(pte_get_entry(&b, 42, NULL)) == 7);

    assert(pte_set_fpn(&a, 42, 8192, &owner) == -1);
    assert(pte_set_swap(&owner, 42, 3, 0x12345) == 0);
    entry = pte_get_entry(&a, 42, &owner);
    assert(!PAGING_PAGE_PRESENT(entry) && (entry & PAGING_PTE_SWAPPED_MASK));
    assert(PAGING_PTE_SWPTYP(entry) == 3 && PAGING_PTE_SWP(entry) == 0x12345);
    assert(pte_set_fpn(&a, 42, 2, &owner) == 0);
    entry = pte_get_entry(&a, 42, &owner);
    assert(entry == (PAGING_PTE_PRESENT_MASK | 2));
    assert(pte_set_entry(&a, 42, 0, &owner) == 0);
    assert(pte_get_entry(&a, 42, &owner) == 0);
    assert(pte_set_entry(&a, UINT64_MAX, 0, NULL) == -1);
    assert(pte_set_fpn(&a, high >> 12, 9, &owner) == 0);
    assert(PAGING_FPN(pte_get_entry(&a, high >> 12, &owner)) == 9);

    struct framephy_struct frame1 = {.fpn = 0};
    struct framephy_struct frame2 = {.fpn = 17};
    struct vm_rg_struct region = {.rg_start = 1, .rg_end = 2};
    const addr_t before = a.page_table_bytes;
    assert(vmap_page_range(&owner, 0x1ff000, 2, &frame1, &region) == (addr_t)-1);
    assert(a.page_table_bytes == before && a.fifo_pgn == NULL);
    assert(region.rg_start == 1 && region.rg_end == 2);
    frame1.fp_next = &frame2;
    assert(vmap_page_range(&owner, 0x1ff001, 2, &frame1, &region) == (addr_t)-1);
    assert(vmap_page_range(&owner, 0x1ff000, 2, &frame1, &region) == 0);
    assert(region.rg_start == 0x1ff000 && region.rg_end == 0x201000);
    assert(pte_get_entry(&a, 511, NULL) == PAGING_PTE_PRESENT_MASK);
    assert(PAGING_FPN(pte_get_entry(&a, 512, NULL)) == 17);
    assert(a.fifo_pgn->pgn == 512 && a.fifo_pgn->pg_next->pgn == 511);
    assert(a.fifo_pgn->pg_next->pg_next == NULL);
    assert(vmap_page_range(&owner, 0x1fe000, 2, &frame1, &region) == (addr_t)-1);
    assert(pte_get_entry(&a, 510, NULL) == 0);
    assert(vmap_page_range(&owner, UINT64_C(0x00fffffffffff000), 2, &frame1, &region) == (addr_t)-1);

    const addr_t mapped_bytes = a.page_table_bytes;
    fail_calloc_after = 2;
    assert(vmap_page_range(&owner, 0x3ffff000, 2, &frame1, &region) == (addr_t)-1);
    fail_calloc_after = -1;
    assert(a.page_table_bytes == mapped_bytes);
    assert(pte_get_entry(&a, 0x3ffff, NULL) == 0);
    assert(pte_get_entry(&a, 0x40000, NULL) == 0);

    const struct thread_args first = {.mm = &b, .first = 512};
    const struct thread_args second = {.mm = &b, .first = 640};
    pthread_t one, two;
    assert(pthread_create(&one, NULL, write_pages, (void *)&first) == 0);
    assert(pthread_create(&two, NULL, write_pages, (void *)&second) == 0);
    assert(pthread_join(one, NULL) == 0 && pthread_join(two, NULL) == 0);
    assert(b.pte_lookup_reads == 5 * 129);
    assert(b.page_table_bytes == 6 * 4096);
    assert(print_pgtbl(&owner, 0x1ff000, 0x201000) == 0);
    assert(print_pgtbl(&owner, 0, UINT64_MAX) == 0);

    destroy_mm(&a);
    destroy_mm(&b);
    assert(a.pgd == NULL && a.mmap == NULL && a.page_table_bytes == 0);
    destroy_mm(&a);
    puts("PASS: 5 levels, offset, isolation, PTE state, range rollback, stats and concurrent access");
    return 0;
}
