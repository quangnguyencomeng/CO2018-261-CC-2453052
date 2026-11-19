/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System Library
 * Memory Module Library libmem.c
 */

// #ifdef MM_PAGING

#include "string.h"
#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
void destroy_mm(struct mm_struct *mm);

static int free_kernel_region(struct pcb_t *caller, struct vm_rg_struct *region);

static int region_fits(const struct vm_rg_struct *region, int mode, addr_t offset, addr_t size)
{
  if (region == NULL || region->mode_bit != mode || region->rg_start >= region->rg_end)
    return 0;

  addr_t length = region->rg_end - region->rg_start;
  return offset <= length && size <= length - offset;
}

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  struct vm_rg_struct *rg_node = mm->mmap->vm_freerg_list;

  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  if (rg_node != NULL)
    rg_elmt->rg_next = rg_node;

  /* Enlist the new region */
  mm->mmap->vm_freerg_list = rg_elmt;

  return 0;
}

/*get_symrg_byid - get mem region by region ID
 *@mm: memory region
 *@rgid: region ID act as symbol index of variable
 *
 */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
  if (mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return NULL;

  return &mm->symrgtbl[rgid];
}

/*__alloc - allocate a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *@alloc_addr: address of allocated memory region
 *
 */
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /*Allocate at the toproof */
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct rgnode;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  if (currg == NULL || cur_vma == NULL || alloc_addr == NULL ||
      size == 0 || size > INT_MAX || currg->rg_start < currg->rg_end)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  if (get_free_vmrg_area(caller, vmaid, (int)size, &rgnode) == 0)
  {
    currg->rg_start = rgnode.rg_start;
    currg->rg_end = rgnode.rg_end;
    currg->vmaid = vmaid;
    currg->mode_bit = 1;
 
    *alloc_addr = rgnode.rg_start;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  /* TODO get_free_vmrg_area FAILED handle the region management (Fig.6)*/

  /*Attempt to increate limit to get space */
  addr_t old_sbrk = cur_vma->sbrk;
  addr_t new_end = old_sbrk + size;
  if (new_end < old_sbrk)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* TODO INCREASE THE LIMIT
   * SYSCALL 1 sys_memmap
   */
  struct sc_regs regs = {0};
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
#ifdef MM64
  regs.a3 = size;
#else
  regs.a3 = PAGING_PAGE_ALIGNSZ(size);
#endif  
  int ret = _syscall(caller->krnl, caller->pid, 17, &regs); /* SYSCALL 17 sys_memmap */
  if (ret != 0 || cur_vma->sbrk < new_end || cur_vma->vm_end < new_end)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  /*Successful increase limit */
  currg->rg_start = old_sbrk;
  currg->rg_end = new_end;
  currg->vmaid = vmaid;
  currg->mode_bit = 1;

  *alloc_addr = old_sbrk;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;

}

/*__free - remove a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __free(struct pcb_t *caller, int vmaid, int rgid)
{
  pthread_mutex_lock(&mmvm_lock);

  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* TODO: Manage the collect freed region to freerg_list */
  struct vm_rg_struct *rgnode = get_symrg_byid(caller->mm, rgid);

  if (rgnode == NULL || rgnode->vmaid != vmaid || rgnode->rg_start >= rgnode->rg_end)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  if (rgnode->mode_bit == 0)
  {
    int ret = free_kernel_region(caller, rgnode);
    pthread_mutex_unlock(&mmvm_lock);
    return ret;
  }

  if (caller->mm->mmap == NULL)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  struct vm_rg_struct *freerg_node = malloc(sizeof(struct vm_rg_struct));
  if (freerg_node == NULL)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  *freerg_node = *rgnode;
  freerg_node->rg_next = NULL;

  /*enlist the obsoleted memory region */
  if (enlist_vm_freerg_list(caller->mm, freerg_node) != 0)
  {
    free(freerg_node);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  *rgnode = (struct vm_rg_struct){0};

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*liballoc - PAGING-based allocate a region memory
 *@proc:  Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  addr_t  addr;
  int val = __alloc(proc, 0, reg_index, size, &addr);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  /* By default using vmaid = 0 */
  return val;
}

/*libfree - PAGING-based free a region memory
 *@proc: Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */

int libfree(struct pcb_t *proc, uint32_t reg_index)
{
  int val = __free(proc, 0, reg_index);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif
  return 0;//val;
}

static struct pgn_t **victim_page_link(struct mm_struct *mm)
{
  struct pgn_t **link = &mm->fifo_pgn;
  while (*link != NULL && (*link)->pg_next != NULL)
    link = &(*link)->pg_next;
  return link;
}

/*pg_getpage - get the page in ram
 *@mm: memory region
 *@pagenum: PGN
 *@framenum: return FPN
 *@caller: caller
 *
 */
int pg_getpage(struct mm_struct *mm, addr_t pgn, addr_t *fpn, struct pcb_t *caller)
{
  struct krnl_t *krnl = caller->krnl;
  struct memphy_struct *ram = krnl->mram;
  if (mm == NULL || fpn == NULL || ram == NULL || ram->maxsz < PAGING64_PAGESZ)
    return -1;

  addr_t pte = pte_get_entry(mm, pgn, caller);
  if (PAGING_PAGE_PRESENT(pte))
  {
    *fpn = PAGING_FPN(pte);
    return *fpn < (addr_t)ram->maxsz / PAGING64_PAGESZ ? 0 : -1;
  }

  else
  { /* Page is not online, make it actively living */
    struct memphy_struct *swap = krnl->active_mswp;
    if (mm != caller->mm || !(pte & PAGING_PTE_SWAPPED_MASK) ||
        swap == NULL || swap->maxsz < PAGING64_PAGESZ ||
        PAGING_PTE_SWPTYP(pte) != krnl->active_mswp_id)
      return -1;

    /* TODO Initialize the target frame storing our variable */
//  addr_t tgtfpn 
    addr_t tgtfpn = PAGING_SWP(pte);
    if (tgtfpn >= (addr_t)swap->maxsz / PAGING64_PAGESZ)
      return -1;

    addr_t ramfpn;
    if (MEMPHY_get_freefp(ram, &ramfpn) == 0)
    {
      struct pgn_t *page = malloc(sizeof *page);
      if (page == NULL || ramfpn >= (addr_t)ram->maxsz / PAGING64_PAGESZ ||
          __swap_cp_page(swap, tgtfpn, ram, ramfpn) != 0 ||
          pte_set_fpn(mm, pgn, ramfpn, caller) != 0 ||
          MEMPHY_put_freefp(swap, tgtfpn) != 0)
      {
        pte_set_entry(mm, pgn, pte, caller);
        MEMPHY_put_freefp(ram, ramfpn);
        free(page);
        return -1;
      }

      page->pgn = pgn;
      page->pg_next = mm->fifo_pgn;
      mm->fifo_pgn = page;
      *fpn = ramfpn;
      return 0;
    }

    /* TODO: Play with your paging theory here */
    /* Find victim page */
    struct pgn_t **link = victim_page_link(mm);
    struct pgn_t *victim = *link;
    if (victim == NULL)
      return -1;

    addr_t vicpgn = victim->pgn;
    addr_t vicpte = pte_get_entry(mm, vicpgn, caller);
    addr_t vicfpn = PAGING_FPN(vicpte);
    if (!PAGING_PAGE_PRESENT(vicpte) || vicfpn >= (addr_t)ram->maxsz / PAGING64_PAGESZ)
      return -1;

    /* Get free frame in MEMSWP */
    addr_t swpfpn;
    if (MEMPHY_get_freefp(swap, &swpfpn) != 0)
      return -1;

    /* TODO: Implement swap frame from MEMRAM to MEMSWP and vice versa*/

    /* TODO copy victim frame to swap 
     * SWP(vicfpn <--> swpfpn)
     * SYSCALL 1 sys_memmap
     */
    struct sc_regs regs = {0};
    regs.a1 = SYSMEM_SWP_OP;
    regs.a2 = vicfpn;
    regs.a3 = swpfpn;
    if (swpfpn >= (addr_t)swap->maxsz / PAGING64_PAGESZ ||
        _syscall(krnl, caller->pid, 17, &regs) != 0)
    {
      MEMPHY_put_freefp(swap, swpfpn);
      return -1;
    }

    /* Update page table */
    //pte_set_swap(...);

    /* Update its online status of the target page */
    //pte_set_fpn(...);
    if (__swap_cp_page(swap, tgtfpn, ram, vicfpn) != 0 ||
        pte_set_swap(caller, vicpgn, krnl->active_mswp_id, swpfpn) != 0 ||
        pte_set_fpn(mm, pgn, vicfpn, caller) != 0 ||
        MEMPHY_put_freefp(swap, tgtfpn) != 0)
    {
      __swap_cp_page(swap, swpfpn, ram, vicfpn);
      pte_set_entry(mm, vicpgn, vicpte, caller);
      pte_set_entry(mm, pgn, pte, caller);
      MEMPHY_put_freefp(swap, swpfpn);
      return -1;
    }

    *link = NULL;
    victim->pgn = pgn;
    victim->pg_next = mm->fifo_pgn;
    mm->fifo_pgn = victim;
    *fpn = vicfpn;
    return 0;
  }
}

/*pg_getval - read value at given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_getval(struct mm_struct *mm, addr_t addr, BYTE *data, struct pcb_t *caller)
{
  addr_t pgn = addr >> PAGING64_ADDR_PT_SHIFT;
//int off = ...
  addr_t off = addr % PAGING64_PAGESZ;
  addr_t fpn;
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */
//int phyaddr = ...

  /* TODO 
   *  MEMPHY_read(caller->krnl->mram, phyaddr, data);
   *  MEMPHY READ 
   *  SYSCALL 17 sys_memmap with SYSMEM_IO_READ
   */
  struct sc_regs regs = {0};
  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = fpn * PAGING64_PAGESZ + off;
  if (_syscall(caller->krnl, caller->pid, 17, &regs) != 0)
    return -1;

  *data = (BYTE)regs.a3;
  return 0;
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_setval(struct mm_struct *mm, addr_t addr, BYTE value, struct pcb_t *caller)
{
  addr_t pgn = addr >> PAGING64_ADDR_PT_SHIFT;
//int off = ...
  addr_t off = addr % PAGING64_PAGESZ;
  addr_t fpn;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  /* TODO 
   *  MEMPHY_write(caller->krnl->mram, phyaddr, value);
   *  MEMPHY WRITE with SYSMEM_IO_WRITE 
   * SYSCALL 17 sys_memmap
   */
  struct sc_regs regs = {0};
  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = fpn * PAGING64_PAGESZ + off;
  regs.a3 = (unsigned char)value;
  return _syscall(caller->krnl, caller->pid, 17, &regs) == 0 ? 0 : -1;
}

/*__read - read value in region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  pthread_mutex_lock(&mmvm_lock);

//struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);

  /* TODO Invalid memory identify */
  int ret = __read_user_mem(caller, vmaid, rgid, offset, data);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*libread - PAGING-based read a region memory */
int libread(
    struct pcb_t *proc, // Process executing the instruction
    uint32_t source,    // Index of source register
    addr_t offset,    // Source address = [source] + [offset]
    uint32_t* destination)
{
  if (destination == NULL)
    return -1;

  BYTE data;
  int val = __read(proc, 0, source, offset, &data);

  if (val != 0)
    return val;

  *destination = (unsigned char)data;
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}

/*__write - write a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  pthread_mutex_lock(&mmvm_lock);
  int ret = __write_user_mem(caller, vmaid, rgid, offset, value); /* Invalid memory identify */
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,   // Process executing the instruction
    BYTE data,            // Data to be wrttien into memory
    uint32_t destination, // Index of destination register
    addr_t offset)
{
  int val = __write(proc, 0, destination, offset, data);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}


/*libkmem_malloc- alloc region memory in kmem
 *@caller: caller
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 */

int libkmem_malloc(struct pcb_t * caller, uint32_t size, uint32_t reg_index)
{
  /* 
   * TODO: provide OS level management
   *       and forward the request to helper
   */
//addr_t  addr;
//int val = ...

  /* 
   * TODO: provide OS kmem allocation validation
   */
  addr_t addr;
  return __kmalloc(caller, 0, reg_index, size, &addr) == 0 ? 0 : -1;
}


/*kmalloc - alloc region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 *@alloc_addr: allocated address
 */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /* TODO: provide OS kernel memory allocation
   *       update krnl_pgd for OS kernel level management */

  //struct krnl_t *krnl = caller->krnl;
  //krnl->symrgtbl...
  //krnl->krnl_pgd ...
  if (caller == NULL || caller->krnl == NULL || alloc_addr == NULL || size == 0)
    return (addr_t)-1;

  pthread_mutex_lock(&mmvm_lock);
  struct mm_struct *mm = caller->krnl->mm;
  struct memphy_struct *ram = caller->krnl->mram;
  struct vm_rg_struct *region = get_symrg_byid(caller->mm, rgid);
  addr_t result = (addr_t)-1;
  if (mm == NULL || mm == caller->mm || mm->pgd == NULL || mm->mmap == NULL ||
      ram == NULL || ram->storage == NULL || ram->maxsz <= 0 ||
      region == NULL || region->rg_start < region->rg_end)
    goto out;

  struct vm_area_struct *vma = mm->mmap;
  if (vma->vm_id != (unsigned long)vmaid)
    goto out;

  addr_t frame_count = (addr_t)ram->maxsz / PAGING64_PAGESZ;
  addr_t pte_frames = PAGING_FPN((addr_t)-1) + 1;
  if (frame_count > pte_frames)
    frame_count = pte_frames;
  if (size > frame_count * PAGING64_PAGESZ)
    goto out;

  addr_t kernel_start = (addr_t)0 - BIT_ULL(PAGING64_CPU_BUS_WIDTH - 1);
  addr_t start = vma->sbrk == 0 ? kernel_start : vma->sbrk;
  addr_t span = PAGING64_PAGE_ALIGNSZ(size);
  if (start < kernel_start || start % PAGING64_PAGESZ != 0 || span > (addr_t)-1 - start)
    goto out;

  unsigned char *available = calloc((size_t)frame_count, sizeof *available);
  if (available == NULL)
    goto out;

  for (struct framephy_struct *node = ram->free_fp_list; node != NULL; node = node->fp_next)
    if (node->fpn < frame_count)
      available[node->fpn] = 1;

  addr_t pages = span / PAGING64_PAGESZ;
  addr_t run = 0, fpn = 0;
  for (; fpn < frame_count && run < pages; fpn++)
    run = available[fpn] ? run + 1 : 0;
  free(available);
  if (run < pages)
    goto out;

  addr_t first_fpn = fpn - pages;
  addr_t pgn = start >> PAGING64_ADDR_PT_SHIFT;
  addr_t mapped = 0;
  for (; mapped < pages; mapped++)
    if (pte_get_entry(mm, pgn + mapped, caller) != 0 ||
        pte_set_fpn(mm, pgn + mapped, first_fpn + mapped, caller) != 0)
      break;

  if (mapped != pages)
  {
    while (mapped > 0)
      pte_set_entry(mm, pgn + --mapped, 0, caller);
    goto out;
  }

  struct framephy_struct **link = &ram->free_fp_list;
  while (*link != NULL)
  {
    struct framephy_struct *node = *link;
    if (first_fpn <= node->fpn && node->fpn < first_fpn + pages)
    {
      *link = node->fp_next;
      free(node);
    }
    else
      link = &node->fp_next;
  }

  memset(ram->storage + first_fpn * PAGING64_PAGESZ, 0, (size_t)span);
  if (vma->sbrk == 0)
    vma->vm_start = kernel_start;
  vma->sbrk = vma->vm_end = start + span;
  region->vmaid = vmaid;
  region->mode_bit = 0;
  region->rg_start = start;
  region->rg_end = start + size;
  *alloc_addr = start;
  result = 0;

out:
  pthread_mutex_unlock(&mmvm_lock);
  return result;
}



/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller
 *@src: source region
 *@dst: destination region
 *@offset: memory offset
 *@size: copied data size
 */
int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst, uint32_t offset, uint32_t size)
{
  /* TODO: provide DMA mempy */

  //struct krnl_t *krnl = caller->krnl;

  /*
   *  TODO: provide asynchronous dma memcpy
   *        handling the dma_done_callback
   */
  //do_dma(caller, (uint64_t) src, (uint64_t) dst, (uint64_t) size);
  //krnl->...
  //krnl->...

  return 0;
}


int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_user_mem(...)
  //__write_kernel_mem(...);
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct *src = get_symrg_byid(caller->mm, source);
  struct vm_rg_struct *dst = get_symrg_byid(caller->mm, destination);
  int ret = -1;
  if (region_fits(src, 1, offset, size) && region_fits(dst, 0, offset, size))
  {
    ret = 0;
    for (addr_t i = 0; i < size; i++)
    {
      BYTE data;
      if (__read_user_mem(caller, src->vmaid, source, (addr_t)offset + i, &data) != 0 ||
          __write_kernel_mem(caller, dst->vmaid, destination, (addr_t)offset + i, data) != 0)
      {
        ret = -1;
        break;
      }
    }
  }
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_kernel_mem(...)
  //__write_user_mem(...);
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct *src = get_symrg_byid(caller->mm, source);
  struct vm_rg_struct *dst = get_symrg_byid(caller->mm, destination);
  int ret = -1;
  if (region_fits(src, 0, offset, size) && region_fits(dst, 1, offset, size))
  {
    ret = 0;
    for (addr_t i = 0; i < size; i++)
    {
      BYTE data;
      if (__read_kernel_mem(caller, src->vmaid, source, (addr_t)offset + i, &data) != 0 ||
          __write_user_mem(caller, dst->vmaid, destination, (addr_t)offset + i, data) != 0)
      {
        ret = -1;
        break;
      }
    }
  }
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}


/*__read_kernel_mem - read value in kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy
  struct vm_rg_struct *region = get_symrg_byid(caller->mm, rgid);
  if (!region_fits(region, 0, offset, 1) || region->vmaid != vmaid || data == NULL)
    return -1;

  return pg_getval(caller->krnl->mm, region->rg_start + offset, data, caller);
}

/*__write_kernel_mem - write a kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy
  struct vm_rg_struct *region = get_symrg_byid(caller->mm, rgid);
  if (!region_fits(region, 0, offset, 1) || region->vmaid != vmaid)
    return -1;

  return pg_setval(caller->krnl->mm, region->rg_start + offset, value, caller);
}

/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy
  struct vm_rg_struct *region = get_symrg_byid(caller->mm, rgid);
  if (!region_fits(region, 1, offset, 1) || region->vmaid != vmaid || data == NULL)
    return -1;

  return pg_getval(caller->mm, region->rg_start + offset, data, caller);
}


/*__write_user_mem - write a user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy
  struct vm_rg_struct *region = get_symrg_byid(caller->mm, rgid);
  if (!region_fits(region, 1, offset, 1) || region->vmaid != vmaid)
    return -1;

  return pg_setval(caller->mm, region->rg_start + offset, value, caller);
}


static int free_kernel_region(struct pcb_t *caller, struct vm_rg_struct *region)
{
  struct mm_struct *mm = caller->krnl->mm;
  struct memphy_struct *ram = caller->krnl->mram;
  if (mm == NULL || ram == NULL || ram->storage == NULL || ram->maxsz < PAGING64_PAGESZ)
    return -1;

  addr_t pgn = region->rg_start >> PAGING64_ADDR_PT_SHIFT;
  addr_t last = (region->rg_end - 1) >> PAGING64_ADDR_PT_SHIFT;
  for (; pgn <= last; pgn++)
  {
    addr_t pte = pte_get_entry(mm, pgn, caller);
    if (pte == 0)
      continue;
    addr_t fpn = PAGING_FPN(pte);
    if (!PAGING_PAGE_PRESENT(pte) || fpn >= (addr_t)ram->maxsz / PAGING64_PAGESZ ||
        pte_set_entry(mm, pgn, 0, caller) != 0)
      return -1;
    if (MEMPHY_put_freefp(ram, fpn) != 0)
    {
      pte_set_entry(mm, pgn, pte, caller);
      return -1;
    }
    memset(ram->storage + fpn * PAGING64_PAGESZ, 0, PAGING64_PAGESZ);
  }
  *region = (struct vm_rg_struct){0};
  return 0;
}

static int free_page_frames(struct krnl_t *krnl, addr_t *table, int levels)
{
  const addr_t entries = BIT(PAGING64_ADDR_PT_HIBIT - PAGING64_ADDR_PT_LOBIT + 1);
  for (addr_t i = 0; i < entries; i++)
  {
    addr_t pte = table[i];
    if (pte == 0)
      continue;
    if (levels > 1)
    {
      if (free_page_frames(krnl, (addr_t *)(uintptr_t)pte, levels - 1) != 0)
        return -1;
      continue;
    }

    struct memphy_struct *device;
    addr_t fpn;
    if (PAGING_PAGE_PRESENT(pte))
    {
      device = krnl->mram;
      fpn = PAGING_FPN(pte);
    }
    else if (pte & PAGING_PTE_SWAPPED_MASK)
    {
      if (PAGING_PTE_SWPTYP(pte) != krnl->active_mswp_id)
        return -1;
      device = krnl->active_mswp;
      fpn = PAGING_SWP(pte);
    }
    else
      continue;

    if (device == NULL || device->maxsz < PAGING64_PAGESZ ||
        fpn >= (addr_t)device->maxsz / PAGING64_PAGESZ ||
        MEMPHY_put_freefp(device, fpn) != 0)
      return -1;
    if (device == krnl->mram && device->storage != NULL)
      memset(device->storage + fpn * PAGING64_PAGESZ, 0, PAGING64_PAGESZ);
    table[i] = 0;
  }
  return 0;
}

/*free_pcb_memphy - collect all memphy of pcb
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 */
int free_pcb_memph(struct pcb_t *caller)
{
  if (caller == NULL || caller->krnl == NULL || caller->mm == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  struct mm_struct *mm = caller->mm;
  int ret = 0;
  for (int rgid = 0; rgid < PAGING_MAX_SYMTBL_SZ && ret == 0; rgid++)
  {
    struct vm_rg_struct *region = &mm->symrgtbl[rgid];
    if (region_fits(region, 0, 0, 1))
      ret = free_kernel_region(caller, region);
  }

  if (ret == 0 && mm->pgd != NULL)
    ret = free_page_frames(caller->krnl, mm->pgd, 5);
  if (ret == 0)
    destroy_mm(mm);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}


/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  /* TODO: Implement the theorical mechanism to find the victim page */
  struct pgn_t **link = victim_page_link(mm);
  struct pgn_t *pg = *link;
  if (pg == NULL)
    return -1;

  *retpgn = pg->pgn;
  *link = NULL;
  free(pg);
  return 0;
}

/*get_free_vmrg_area - get a free vm region
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: allocated size
 *
 */
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size, struct vm_rg_struct *newrg)
{
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = -1;

  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL)
  {
    if (rgit->rg_start <= rgit->rg_end && (addr_t)size <= rgit->rg_end - rgit->rg_start)
    { /* Current region has enough space */
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;

      /* Update left space in chosen region */
      if (rgit->rg_start + size < rgit->rg_end)
      {
        rgit->rg_start = rgit->rg_start + size;
      }
      else
      { /*Use up all space, remove current node */
        /*Clone next rg node */
        struct vm_rg_struct *nextrg = rgit->rg_next;

        /*Cloning */
        if (nextrg != NULL)
        {
          rgit->rg_start = nextrg->rg_start;
          rgit->rg_end = nextrg->rg_end;

          rgit->rg_next = nextrg->rg_next;

          free(nextrg);
        }
        else
        {                                /*End of free list */
          rgit->rg_start = rgit->rg_end; // dummy, size 0 region
          rgit->rg_next = NULL;
        }
      }
      break;
    }
    else
    {
      rgit = rgit->rg_next; // Traverse next rg
    }
  }

  if (newrg->rg_start == -1) // new region not found
    return -1;

  return 0;
}

// #endif
