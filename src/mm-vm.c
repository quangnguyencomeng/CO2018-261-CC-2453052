/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

//#ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Virtual memory module mm/mm-vm.c
 */

#include "string.h"
#include "mm64.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* get_vma_by_num - get vm area by numID
 * @mm: memory region
 * @vmaid: ID vm area to alloc memory region
 */
struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
  if (mm == NULL || mm->mmap == NULL)
    return NULL;

  struct vm_area_struct *pvma = mm->mmap;
  while (pvma != NULL)
  {
    if ((int)pvma->vm_id == vmaid)
      return pvma;
    pvma = pvma->vm_next;
  }

  return NULL;
}

int __mm_swap_page(struct pcb_t *caller, addr_t vicfpn, addr_t swpfpn)
{
  if (caller == NULL || caller->krnl == NULL)
    return -1;

  return __swap_cp_page(caller->krnl->mram, vicfpn,
                        caller->krnl->active_mswp, swpfpn);
}

/* get_vm_area_node_at_brk - allocate a candidate vm_rg_struct at current sbrk
 * @caller: caller PCB
 * @vmaid: ID vm area to expand
 * @alignedsz: page-aligned expansion size
 */
struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller, int vmaid,
                                             addr_t alignedsz)
{
  if (caller == NULL || caller->mm == NULL)
    return NULL;

  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (cur_vma == NULL)
    return NULL;

  struct vm_rg_struct *newrg =
      (struct vm_rg_struct *)malloc(sizeof(struct vm_rg_struct));
  if (newrg == NULL)
    return NULL;

  newrg->vmaid = vmaid;
  newrg->rg_start = cur_vma->sbrk;
  newrg->rg_end = newrg->rg_start + alignedsz;
  newrg->rg_next = NULL;

  return newrg;
}

/* validate_overlap_vm_area - verify planned range [vmastart, vmaend) does not
 * overlap any other VM area in caller->mm
 * @caller: caller PCB
 * @vmaid: ID vm area being expanded
 * @vmastart: planned start address
 * @vmaend: planned end address
 */
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid,
                             addr_t vmastart, addr_t vmaend)
{
  if (caller == NULL || caller->mm == NULL)
    return -1;

  struct vm_area_struct *vma = caller->mm->mmap;

  if (vmastart >= vmaend)
    return -1;

  if (vma == NULL)
    return -1;

  struct vm_area_struct *cur_area = get_vma_by_num(caller->mm, vmaid);
  if (cur_area == NULL)
    return -1;

  while (vma != NULL)
  {
    if (vma != cur_area &&
        OVERLAP(vmastart, vmaend, vma->vm_start, vma->vm_end))
    {
      return -1;
    }
    vma = vma->vm_next;
  }

  return 0;
}

/* inc_vma_limit - increase vm area limits to reserve space for new variable
 * @caller: caller PCB
 * @vmaid: ID vm area to expand
 * @inc_sz: requested increment size in bytes
 */
int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t inc_sz)
{
  if (caller == NULL || caller->mm == NULL)
    return -1;

  if (inc_sz == 0)
    return 0;

#ifdef MM64
  addr_t inc_amt = PAGING64_PAGE_ALIGNSZ(inc_sz);
  int incnumpage = (int)(inc_amt / PAGING64_PAGESZ);
#else
  addr_t inc_amt = PAGING_PAGE_ALIGNSZ(inc_sz);
  int incnumpage = (int)(inc_amt / PAGING_PAGESZ);
#endif

  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (cur_vma == NULL)
    return -1;

  struct vm_rg_struct *area = get_vm_area_node_at_brk(caller, vmaid, inc_amt);
  if (area == NULL)
    return -1;

  addr_t old_end = cur_vma->vm_end;
  addr_t old_sbrk = cur_vma->sbrk;
  addr_t map_start = old_end;
  addr_t map_end = old_end + inc_amt;

  /* Guard against 64-bit address wraparound */
  if (map_end < map_start)
  {
    free(area);
    return -1;
  }

  /* Validate overlap of planned expansion region against other VMAs */
  if (validate_overlap_vm_area(caller, vmaid, area->rg_start, area->rg_end) < 0 ||
      validate_overlap_vm_area(caller, vmaid, map_start, map_end) < 0)
  {
    free(area);
    return -1; /* Overlap and failed allocation */
  }

  struct vm_rg_struct newrg;
  memset(&newrg, 0, sizeof(newrg));
  newrg.vmaid = vmaid;

  /* Map newly requested physical frames into RAM before advancing VMA limits */
  if ((int64_t)vm_map_ram(caller, map_start, map_end,
                          map_start, incnumpage, &newrg) < 0)
  {
    free(area);
    return -1; /* Mapping failed (e.g. OOM); VMA limits remain unchanged */
  }

  /* Update VMA limits after successful physical mapping */
  cur_vma->vm_end = map_end;
  cur_vma->sbrk = map_end;

  /* Enlist leftover page-aligned space (if inc_sz was not page-aligned) */
  if (old_sbrk + inc_sz < cur_vma->vm_end)
  {
    struct vm_rg_struct *leftover =
        init_vm_rg(old_sbrk + inc_sz, cur_vma->vm_end);
    if (leftover != NULL)
    {
      leftover->vmaid = vmaid;
      enlist_vm_rg_node(&cur_vma->vm_freerg_list, leftover);
    }
  }

  free(area);
  return 0;
}

// #endif
