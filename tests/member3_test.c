#include "common.h"
#include "libmem.h"
#include "mm64.h"
#include "queue.h"
#include "sched.h"
#include "syscall.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller,
                                                    int vmaid,
                                                    addr_t alignedsz);

/*
 * Minimal __read stub for standalone testing on main before Member 4 merges.
 * Uses Member 2's 5-level pte_get_entry() and Member 3's MEMPHY_read().
 */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
   (void)vmaid;
   if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
       rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ || data == NULL)
      return -1;

   struct vm_rg_struct *rg = &caller->mm->symrgtbl[rgid];
   addr_t vaddr = rg->rg_start + offset;
   if (vaddr >= rg->rg_end)
      return -1;

   addr_t pgn = vaddr >> PAGING64_ADDR_PT_SHIFT;
   addr_t off = PAGING64_ADDR_OFFST(vaddr);
   addr_t pte = pte_get_entry(caller->mm, pgn, caller);
   if (!PAGING_PAGE_PRESENT(pte))
      return -1;

   addr_t fpn = PAGING_FPN(pte);
   return MEMPHY_read(caller->krnl->mram, fpn * PAGING64_PAGESZ + off, data);
}

static void cleanup_memphy(struct memphy_struct *mp)
{
   struct framephy_struct *it = mp->free_fp_list;
   while (it != NULL)
   {
      struct framephy_struct *next = it->fp_next;
      free(it);
      it = next;
   }
   mp->free_fp_list = NULL;
   free(mp->storage);
   mp->storage = NULL;
}

static void test_memphy(void)
{
   addr_t fpn = 99;
   BYTE b = 0;

   /* 0-byte inactive swap device (e.g. mswp[1..3]) */
   struct memphy_struct mp0;
   memset(&mp0, 0xAA, sizeof(mp0));
   assert(init_memphy(&mp0, 0, 1) == 0);
   assert(mp0.free_fp_list == NULL);
   assert(MEMPHY_get_freefp(&mp0, &fpn) == -1);
   cleanup_memphy(&mp0);

   /* 2048-byte RAM (os_1_mlq_paging_small_1K: 0 full 4KB frames) */
   struct memphy_struct mp1k;
   memset(&mp1k, 0xBB, sizeof(mp1k));
   assert(init_memphy(&mp1k, 2048, 1) == 0);
   assert(mp1k.free_fp_list == NULL);
   assert(MEMPHY_get_freefp(&mp1k, &fpn) == -1);
   assert(MEMPHY_write(&mp1k, 2048, 1) == -1);
   assert(MEMPHY_read(&mp1k, 2048, &b) == -1);
   cleanup_memphy(&mp1k);

   /* 4096-byte RAM (os_1_mlq_paging_small_4K: exactly 1 frame) */
   struct memphy_struct mp4k;
   memset(&mp4k, 0xCC, sizeof(mp4k));
   assert(init_memphy(&mp4k, PAGING64_PAGESZ, 1) == 0);
   assert(MEMPHY_get_freefp(&mp4k, &fpn) == 0 && fpn == 0);
   assert(MEMPHY_get_freefp(&mp4k, &fpn) == -1);
   cleanup_memphy(&mp4k);

   /* 4-frame RAM: FIFO free frames, put_freefp, read/write, dump */
   struct memphy_struct mp;
   assert(init_memphy(&mp, 4 * PAGING64_PAGESZ, 1) == 0);
   for (addr_t expect = 0; expect < 4; expect++)
   {
      assert(MEMPHY_get_freefp(&mp, &fpn) == 0);
      assert(fpn == expect);
   }
   assert(MEMPHY_get_freefp(&mp, &fpn) == -1);
   assert(MEMPHY_put_freefp(&mp, 2) == 0);
   assert(MEMPHY_get_freefp(&mp, &fpn) == 0 && fpn == 2);

   assert(MEMPHY_write(&mp, 16, 80) == 0);
   assert(MEMPHY_write(&mp, 17, 48) == 0);
   assert(MEMPHY_read(&mp, 16, &b) == 0 && b == 80);
   assert(MEMPHY_read(&mp, 17, &b) == 0 && b == 48);
   assert(MEMPHY_dump(&mp) == 0);
   cleanup_memphy(&mp);

   /* Sequential device mode (rdmflg = 0) */
   struct memphy_struct mp_seq;
   assert(init_memphy(&mp_seq, PAGING64_PAGESZ, 0) == 0);
   assert(MEMPHY_write(&mp_seq, 64, 55) == 0);
   assert(MEMPHY_read(&mp_seq, 64, &b) == 0 && b == 55);
   cleanup_memphy(&mp_seq);

   printf("[PASS] test_memphy\n");
}

static void test_mm_vm_with_member2(void)
{
   struct memphy_struct mram, mswp;
   assert(init_memphy(&mram, 3 * PAGING64_PAGESZ, 1) == 0);
   assert(init_memphy(&mswp, 4 * PAGING64_PAGESZ, 1) == 0);

   struct krnl_t krnl = {0};
   krnl.mram = &mram;
   krnl.active_mswp = &mswp;

   struct pcb_t proc = {0};
   struct mm_struct mm;
   assert(init_mm(&mm, &proc) == 0);
   proc.pid = 1;
   proc.mm = &mm;
   proc.krnl = &krnl;

   /* get_vma_by_num & get_vm_area_node_at_brk */
   struct vm_area_struct *v0 = get_vma_by_num(proc.mm, 0);
   assert(v0 != NULL && v0->vm_id == 0);
   assert(get_vma_by_num(proc.mm, 1) == NULL);

   struct vm_rg_struct *cand = get_vm_area_node_at_brk(&proc, 0, PAGING64_PAGESZ);
   assert(cand != NULL && cand->rg_start == 0 && cand->rg_end == PAGING64_PAGESZ);
   free(cand);

   /* Add vma1 at [8192, 12288) to verify overlap check */
   struct vm_area_struct *v1 = calloc(1, sizeof(*v1));
   v1->vm_id = 1;
   v1->vm_start = 2 * PAGING64_PAGESZ;
   v1->vm_end = 3 * PAGING64_PAGESZ;
   v1->sbrk = v1->vm_end;
   v1->vm_mm = proc.mm;
   v0->vm_next = v1;

   /* Unaligned inc_vma_limit(300) -> maps 1 page [0, 4096), enlists [300, 4096) */
   assert(inc_vma_limit(&proc, 0, 300) == 0);
   assert(v0->vm_end == PAGING64_PAGESZ && v0->sbrk == PAGING64_PAGESZ);
   assert(PAGING_PAGE_PRESENT(pte_get_entry(proc.mm, 0, &proc)) != 0);
   assert(v0->vm_freerg_list != NULL);
   assert(v0->vm_freerg_list->rg_start == 300);
   assert(v0->vm_freerg_list->rg_end == PAGING64_PAGESZ);

   /* Overlapping expansion [4096, 12288) overlaps v1 -> rejected */
   assert(inc_vma_limit(&proc, 0, 2 * PAGING64_PAGESZ) == -1);
   assert(v0->vm_end == PAGING64_PAGESZ);

   /* Adjacent expansion [4096, 8192) -> allowed */
   assert(inc_vma_limit(&proc, 0, PAGING64_PAGESZ) == 0);
   assert(v0->vm_end == 2 * PAGING64_PAGESZ && v0->sbrk == 2 * PAGING64_PAGESZ);
   assert(PAGING_PAGE_PRESENT(pte_get_entry(proc.mm, 1, &proc)) != 0);

   /* Remove v1 and test OOM rollback (only 1 frame left, request 2 pages) */
   v0->vm_next = NULL;
   free(v1);
   assert(inc_vma_limit(&proc, 0, 2 * PAGING64_PAGESZ) == -1);
   assert(v0->vm_end == 2 * PAGING64_PAGESZ && v0->sbrk == 2 * PAGING64_PAGESZ);

   /* __mm_swap_page copies 4KB frame from RAM to SWAP */
   addr_t fpn0 = PAGING_FPN(pte_get_entry(proc.mm, 0, &proc));
   assert(MEMPHY_write(&mram, fpn0 * PAGING64_PAGESZ + 123, 99) == 0);
   assert(__mm_swap_page(&proc, fpn0, 2) == 0);
   BYTE got = 0;
   assert(MEMPHY_read(&mswp, 2 * PAGING64_PAGESZ + 123, &got) == 0 && got == 99);

   destroy_mm(&mm);
   cleanup_memphy(&mram);
   cleanup_memphy(&mswp);
   printf("[PASS] test_mm_vm_with_member2\n");
}

static void test_syscalls_with_member1_and_member2(void)
{
   struct memphy_struct mram, mswp;
   assert(init_memphy(&mram, 8 * PAGING64_PAGESZ, 1) == 0);
   assert(init_memphy(&mswp, 8 * PAGING64_PAGESZ, 1) == 0);

   struct krnl_t krnl = {0};
   krnl.mram = &mram;
   krnl.active_mswp = &mswp;
   init_scheduler();

   struct code_seg_t code1 = {.text = NULL, .size = 5};
   struct code_seg_t code2 = {.text = NULL, .size = 10};
   struct code_seg_t code3 = {.text = NULL, .size = 10};
   struct code_seg_t code4 = {.text = NULL, .size = 10};

   struct pcb_t p1 = {0}, p2 = {0}, p3 = {0}, p4 = {0};
   struct mm_struct mm1, mm2;
   assert(init_mm(&mm1, &p1) == 0);
   assert(init_mm(&mm2, &p2) == 0);

   p1.pid = 1; p1.prio = 1; snprintf(p1.path, sizeof(p1.path), "input/proc/sc2"); p1.code = &code1; p1.mm = &mm1; p1.krnl = &krnl;
   p2.pid = 2; p2.prio = 2; snprintf(p2.path, sizeof(p2.path), "input/proc/P0");  p2.code = &code2; p2.mm = &mm2; p2.krnl = &krnl;
   p3.pid = 3; p3.prio = 3; snprintf(p3.path, sizeof(p3.path), "input/proc/P0");  p3.code = &code3; p3.krnl = &krnl;
   p4.pid = 4; p4.prio = 4; snprintf(p4.path, sizeof(p4.path), "input/proc/s1");  p4.code = &code4; p4.krnl = &krnl;

   /* Enqueue via Member 1's real scheduler & dispatch p1, p2 into running_list */
   assert(add_proc(&p1) == SCHED_ADD_OK);
   assert(add_proc(&p2) == SCHED_ADD_OK);
   assert(add_proc(&p3) == SCHED_ADD_OK);
   assert(add_proc(&p4) == SCHED_ADD_OK);
   assert(get_proc() == &p1);
   assert(get_proc() == &p2);

   /* 1) Syscall 0: listsyscall */
   assert(syscall_table_size == 4);
   assert(libsyscall(&p1, 0, 0, 0, 0) == 0);

   /* 2) Syscall 440: xxxhandler */
   assert(libsyscall(&p1, 440, 1, 0, 0) == 0);

   /* 3) Syscall 17: SYSMEM_INC_OP resolved strictly by PID 1 */
   assert(libsyscall(&p1, 17, SYSMEM_INC_OP, 0, 100) == 0);
   assert(p1.mm->mmap->vm_end == PAGING64_PAGESZ);
   assert(p2.mm->mmap->vm_end == 0);
   p1.mm->symrgtbl[1].rg_start = 0;
   p1.mm->symrgtbl[1].rg_end = 100;

   /* Unknown PID 999 rejected safely */
   struct sc_regs bad = {.a1 = SYSMEM_INC_OP, .a2 = 0, .a3 = 100};
   assert(_syscall(&krnl, 999, 17, &bad) == -1);

   /* 4) Syscall 17: SYSMEM_MAP_OP via Member 2's vmap_pgd_memset */
   uint64_t before_bytes = p2.mm->page_table_bytes;
   assert(libsyscall(&p2, 17, SYSMEM_MAP_OP, 0x200000ULL, 2) == 0);
   assert(p2.mm->page_table_bytes > before_bytes);

   /* 5) Syscall 17: SYSMEM_IO_WRITE / SYSMEM_IO_READ writing "P0\xff" */
   addr_t fpn1 = PAGING_FPN(pte_get_entry(p1.mm, 0, &p1));
   addr_t base1 = fpn1 * PAGING64_PAGESZ;
   assert(libsyscall(&p1, 17, SYSMEM_IO_WRITE, base1 + 0, 80) == 0);
   assert(libsyscall(&p1, 17, SYSMEM_IO_WRITE, base1 + 1, 48) == 0);
   assert(libsyscall(&p1, 17, SYSMEM_IO_WRITE, base1 + 2, (unsigned char)-1) == 0);

   struct sc_regs rd = {.a1 = SYSMEM_IO_READ, .a2 = base1 + 0, .a3 = 0};
   assert(_syscall(&krnl, 1, 17, &rd) == 0 && (BYTE)rd.a3 == 'P');
   rd.a2 = base1 + 1;
   assert(_syscall(&krnl, 1, 17, &rd) == 0 && (BYTE)rd.a3 == '0');
   rd.a2 = base1 + 2;
   assert(_syscall(&krnl, 1, 17, &rd) == 0 && (BYTE)rd.a3 == (BYTE)-1);

   /* 6) Syscall 17: SYSMEM_SWP_OP */
   assert(libsyscall(&p1, 17, SYSMEM_SWP_OP, fpn1, 1) == 0);
   BYTE swp_b = 0;
   assert(MEMPHY_read(&mswp, 1 * PAGING64_PAGESZ + 0, &swp_b) == 0 && swp_b == 'P');

   /* 7) Syscall 101: killall("P0") terminates running p2 and purges queued p3 */
   assert(libsyscall(&p1, 101, 1, 0, 0) == 0);
   assert(p2.pc == p2.code->size);
   assert(p3.pc == p3.code->size);
   assert(krnl.mlq_ready_queue[3].size == 0);
   assert(p4.pc == 0 && krnl.mlq_ready_queue[4].size == 1);
   assert(p1.pc == 0);

   remove_proc_from_running_list(&p1);
   remove_proc_from_running_list(&p2);
   finish_scheduler();
   destroy_mm(&mm1);
   destroy_mm(&mm2);
   cleanup_memphy(&mram);
   cleanup_memphy(&mswp);
   printf("[PASS] test_syscalls_with_member1_and_member2\n");
}

static void *mt_memphy_worker(void *arg)
{
   struct memphy_struct *mp = (struct memphy_struct *)arg;
   for (int i = 0; i < 500; i++)
   {
      addr_t fpn = 0;
      if (MEMPHY_get_freefp(mp, &fpn) == 0)
      {
         assert(MEMPHY_write(mp, fpn * PAGING64_PAGESZ, (BYTE)(i & 0x7F)) == 0);
         BYTE out = 0;
         assert(MEMPHY_read(mp, fpn * PAGING64_PAGESZ, &out) == 0);
         assert(MEMPHY_put_freefp(mp, fpn) == 0);
      }
   }
   return NULL;
}

static void test_memphy_concurrency(void)
{
   struct memphy_struct mp;
   assert(init_memphy(&mp, 16 * PAGING64_PAGESZ, 1) == 0);

   pthread_t th[4];
   for (int i = 0; i < 4; i++)
      assert(pthread_create(&th[i], NULL, mt_memphy_worker, &mp) == 0);
   for (int i = 0; i < 4; i++)
      pthread_join(th[i], NULL);

   int count = 0;
   addr_t fpn = 0;
   while (MEMPHY_get_freefp(&mp, &fpn) == 0)
      count++;
   assert(count == 16);

   cleanup_memphy(&mp);
   printf("[PASS] test_memphy_concurrency\n");
}

int main(void)
{
   test_memphy();
   test_mm_vm_with_member2();
   test_syscalls_with_member1_and_member2();
   test_memphy_concurrency();
   printf("PASS: Member 3 MEMPHY, VMA, PID-passing syscalls (0/17/101/440) and concurrency\n");
   return 0;
}
