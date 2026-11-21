/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

/*
 * PAGING based Memory Management
 * Memory management unit mm/mm.c
 */

#include "mm64.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(MM64)

enum {
  PAGE_TABLE_LEVELS = 5,
  PAGE_TABLE_INDEX_BITS = 9,
  PAGE_TABLE_ENTRIES = 1 << PAGE_TABLE_INDEX_BITS
};

static pthread_mutex_t page_table_lock = PTHREAD_MUTEX_INITIALIZER;

struct new_page_tables {
  addr_t *link;
  unsigned int levels;
};

static int is_canonical_address(addr_t addr)
{
  const addr_t high_mask = UINT64_MAX << PAGING64_CPU_BUS_WIDTH;
  const addr_t sign_bit = BIT_ULL(PAGING64_CPU_BUS_WIDTH - 1);

  return (addr & high_mask) == ((addr & sign_bit) ? high_mask : 0);
}

int get_pd_from_address(addr_t addr, addr_t *pgd, addr_t *p4d,
                        addr_t *pud, addr_t *pmd, addr_t *pt)
{
  if (!is_canonical_address(addr))
    return -1;

  *pgd = PAGING64_ADDR_PGD(addr);
  *p4d = PAGING64_ADDR_P4D(addr);
  *pud = PAGING64_ADDR_PUD(addr);
  *pmd = PAGING64_ADDR_PMD(addr);
  *pt = PAGING64_ADDR_PT(addr);
  return 0;
}

int get_pd_from_pagenum(addr_t pgn, addr_t *pgd, addr_t *p4d,
                        addr_t *pud, addr_t *pmd, addr_t *pt)
{
  if (pgn > (UINT64_MAX >> PAGING64_ADDR_PT_SHIFT))
    return -1;

  return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT,
                             pgd, p4d, pud, pmd, pt);
}

/* Caller holds page_table_lock. Non-leaf entries store child table pointers. */
static addr_t *walk_page_table(struct mm_struct *mm, addr_t pgn, int create,
                              addr_t **path, struct new_page_tables *added)
{
  addr_t index[PAGE_TABLE_LEVELS];
  addr_t *new_links[PAGE_TABLE_LEVELS - 1];
  unsigned int new_count = 0;
  addr_t *table = mm->pgd;

  if (get_pd_from_pagenum(pgn, &index[0], &index[1], &index[2],
                          &index[3], &index[4]) < 0)
    return NULL;

  for (unsigned int level = 0; table != NULL; ++level)
  {
    if (path != NULL)
      path[level] = table;

    addr_t *entry = &table[index[level]];
    if (level == PAGE_TABLE_LEVELS - 1)
    {
      mm->page_table_bytes += new_count * PAGE_TABLE_ENTRIES * sizeof(*table);
      if (added != NULL && new_count > 0)
      {
        added->link = new_links[0];
        added->levels = new_count;
      }
      return entry;
    }

    addr_t *child = (addr_t *)(uintptr_t)*entry;
    if (child == NULL && create)
    {
      child = calloc(PAGE_TABLE_ENTRIES, sizeof(*child));
      if (child == NULL)
      {
        while (new_count > 0)
        {
          addr_t *link = new_links[--new_count];
          free((void *)(uintptr_t)*link);
          *link = 0;
        }
        return NULL;
      }
      *entry = (addr_t)(uintptr_t)child;
      new_links[new_count++] = entry;
    }
    table = child;
  }

  return NULL;
}

static void free_page_tables(addr_t *table, unsigned int levels)
{
  if (levels > 1)
  {
    for (unsigned int i = 0; i < PAGE_TABLE_ENTRIES; ++i)
    {
      if (table[i] != 0)
        free_page_tables((addr_t *)(uintptr_t)table[i], levels - 1);
    }
  }
  free(table);
}

int init_pte(addr_t *pte, int pre, addr_t fpn, int drt, int swp,
             int swptyp, addr_t swpoff)
{
  addr_t value = 0;

  if (pte == NULL)
    return -1;

  if (swp)
  {
    if (swptyp < 0 ||
        (addr_t)swptyp > PAGING_PTE_SWPTYP(UINT64_MAX) ||
        swpoff > PAGING_PTE_SWP(UINT64_MAX))
      return -1;

    SETBIT(value, PAGING_PTE_SWAPPED_MASK);
    SETVAL(value, (addr_t)swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
    SETVAL(value, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);
  }
  else if (pre)
  {
    if (fpn > PAGING_FPN(UINT64_MAX))
      return -1;

    SETBIT(value, PAGING_PTE_PRESENT_MASK);
    SETVAL(value, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);
  }

  if (value != 0 && drt)
    SETBIT(value, PAGING_PTE_DIRTY_MASK);

  *pte = value;
  return 0;
}

int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{
  addr_t value;

  if (caller == NULL || init_pte(&value, 0, 0, 0, 1, swptyp, swpoff) < 0)
    return -1;

  return pte_set_entry(caller->mm, pgn, value, caller);
}

int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn, struct pcb_t *caller)
{
  addr_t value;

  if (init_pte(&value, 1, fpn, 0, 0, 0, 0) < 0)
    return -1;

  return pte_set_entry(mm, pgn, value, caller);
}

addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
  addr_t *path[PAGE_TABLE_LEVELS] = {NULL};
  addr_t value = 0;
  (void)caller;

  if (mm == NULL)
    return 0;

  pthread_mutex_lock(&page_table_lock);
  addr_t *pte = walk_page_table(mm, pgn, 0, path, NULL);
  if (pte != NULL)
    value = *pte;

  for (unsigned int level = 0; level < PAGE_TABLE_LEVELS && path[level] != NULL; ++level)
    ++mm->pte_lookup_reads;

  pthread_mutex_unlock(&page_table_lock);
  return value;
}

int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller)
{
  addr_t index[PAGE_TABLE_LEVELS];
  (void)caller;

  if (mm == NULL ||
      get_pd_from_pagenum(pgn, &index[0], &index[1], &index[2], &index[3], &index[4]) < 0)
    return -1;

  pthread_mutex_lock(&page_table_lock);
  addr_t *pte = walk_page_table(mm, pgn, pte_val != 0, NULL, NULL);
  if (pte != NULL)
    *pte = pte_val;

  int result = pte != NULL || (pte_val == 0 && mm->pgd != NULL) ? 0 : -1;
  pthread_mutex_unlock(&page_table_lock);
  return result;
}

/* Prepare zeroed slots; preserve existing mappings. */
int vmap_pgd_memset(struct pcb_t *caller, addr_t addr, int pgnum)
{
  if (caller == NULL || caller->mm == NULL || caller->mm->pgd == NULL ||
      pgnum < 0 || PAGING64_ADDR_OFFST(addr) != 0 || !is_canonical_address(addr))
    return -1;
  addr_t size = (addr_t)pgnum * PAGING64_PAGESZ;
  if (size > UINT64_MAX - addr ||
      (size != 0 && (!is_canonical_address(addr + size - 1) ||
       ((addr ^ (addr + size - 1)) & BIT_ULL(PAGING64_CPU_BUS_WIDTH - 1)))))
    return -1;
  if (pgnum == 0)
    return 0;

  struct new_page_tables *added = calloc((size_t)pgnum, sizeof(*added));
  if (added == NULL)
    return -1;
  int result = 0;
  int page;
  struct mm_struct *mm = caller->mm;
  pthread_mutex_lock(&page_table_lock);
  for (page = 0; page < pgnum; ++page)
  {
    addr_t *pte = walk_page_table(mm, (addr >> PAGING64_ADDR_PT_SHIFT) + page,
                                  1, NULL, &added[page]);
    if (pte == NULL || *pte != 0)
    {
      result = -1;
      break;
    }
  }
  if (result != 0)
  {
    for (int i = page; i >= 0; --i)
    {
      if (added[i].link != NULL)
      {
        free_page_tables((addr_t *)(uintptr_t)*added[i].link, added[i].levels);
        *added[i].link = 0;
        mm->page_table_bytes -= added[i].levels * PAGE_TABLE_ENTRIES * sizeof(addr_t);
      }
    }
  }
  pthread_mutex_unlock(&page_table_lock);
  free(added);
  return result;
}

/*
 * Map a page-aligned range using frames already reserved by the caller.
 * On failure, no PTE or FIFO entry is committed and the caller keeps the frames.
 */
addr_t vmap_page_range(struct pcb_t *caller, addr_t addr, int pgnum,
                       struct framephy_struct *frames, struct vm_rg_struct *ret_rg)
{
  struct page_mapping {
    addr_t *pte;
    addr_t value;
    struct new_page_tables added;
  };
  struct pgn_t *new_pages = NULL;
  struct pgn_t *oldest_new_page = NULL;
  addr_t result = (addr_t)-1;

  if (caller == NULL || caller->mm == NULL || ret_rg == NULL ||
      pgnum < 0 || PAGING64_ADDR_OFFST(addr) != 0 || !is_canonical_address(addr))
    return result;

  addr_t size = (addr_t)pgnum * PAGING64_PAGESZ;
  if (size > UINT64_MAX - addr)
    return result;

  addr_t end = addr + size;
  if (size != 0 &&
      (!is_canonical_address(end - 1) ||
       ((addr ^ (end - 1)) & BIT_ULL(PAGING64_CPU_BUS_WIDTH - 1))))
    return result;

  if (pgnum == 0)
  {
    ret_rg->rg_start = addr;
    ret_rg->rg_end = addr;
    return 0;
  }

  struct page_mapping *mappings = calloc((size_t)pgnum, sizeof(*mappings));
  if (mappings == NULL)
    return result;

  struct mm_struct *mm = caller->mm;
  addr_t first_page = addr >> PAGING64_ADDR_PT_SHIFT;
  pthread_mutex_lock(&page_table_lock);

  int page;
  for (page = 0; page < pgnum; ++page)
  {
    addr_t pgn = first_page + page;
    if (frames == NULL || init_pte(&mappings[page].value, 1, frames->fpn, 0, 0, 0, 0) < 0)
      goto out;

    mappings[page].pte = walk_page_table(mm, pgn, 1, NULL, &mappings[page].added);
    if (mappings[page].pte == NULL || *mappings[page].pte != 0)
      goto out;

    struct pgn_t *node = malloc(sizeof(*node));
    if (node == NULL)
      goto out;

    node->pgn = pgn;
    node->pg_next = new_pages;
    new_pages = node;
    if (oldest_new_page == NULL)
      oldest_new_page = node;

    frames = frames->fp_next;
  }

  for (int i = 0; i < pgnum; ++i)
    *mappings[i].pte = mappings[i].value;

  oldest_new_page->pg_next = mm->fifo_pgn;
  mm->fifo_pgn = new_pages;
  new_pages = NULL;

  ret_rg->rg_start = addr;
  ret_rg->rg_end = end;
  result = 0;

out:
  if (result != 0)
  {
    for (int i = page; i >= 0; --i)
    {
      struct new_page_tables *added = &mappings[i].added;
      if (added->link != NULL)
      {
        free_page_tables((addr_t *)(uintptr_t)*added->link, added->levels);
        *added->link = 0;
        mm->page_table_bytes -= added->levels * PAGE_TABLE_ENTRIES * sizeof(addr_t);
      }
    }
  }
  pthread_mutex_unlock(&page_table_lock);
  while (new_pages != NULL)
  {
    struct pgn_t *node = new_pages;
    new_pages = node->pg_next;
    free(node);
  }
  free(mappings);
  return result;
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 * @caller    : caller
 * @req_pgnum : request page num
 * @frm_lst   : frame list
 */

/* Return caller-owned frame nodes under memphy_lock without extra allocation. */
static void return_frames(struct memphy_struct *ram, struct framephy_struct *frames)
{
  MEMPHY_put_freefp_list(ram, frames);
}

addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst)
{
  if (frm_lst == NULL)
    return (addr_t)-1;
  *frm_lst = NULL;
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mram == NULL || req_pgnum < 0)
    return (addr_t)-1;

  struct memphy_struct *ram = caller->krnl->mram;
  struct framephy_struct *head = NULL;
  struct framephy_struct **tail = &head;
  addr_t result = 0;

  for (int page = 0; page < req_pgnum; ++page)
  {
    struct framephy_struct *node = calloc(1, sizeof(*node));
    if (node == NULL)
    {
      result = (addr_t)-1;
      break;
    }
    if (MEMPHY_get_freefp(ram, &node->fpn) != 0)
    {
      free(node);
      result = (addr_t)-3000;
      break;
    }
    node->owner = caller->mm;
    *tail = node;
    tail = &node->fp_next;
  }

  if (result != 0)
    return_frames(ram, head);
  else
    *frm_lst = head;
  return result;
}

addr_t vm_map_ram(struct pcb_t *caller, addr_t astart, addr_t aend,
                  addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg)
{
  struct framephy_struct *frames = NULL;
  if (astart > aend || mapstart < astart || mapstart > aend || incpgnum < 0 ||
      (addr_t)incpgnum > (aend - mapstart) / PAGING64_PAGESZ || ret_rg == NULL)
    return (addr_t)-1;

  addr_t result = alloc_pages_range(caller, incpgnum, &frames);
  if (result != 0)
    return result;

  result = vmap_page_range(caller, mapstart, incpgnum, frames, ret_rg);
  if (result != 0)
  {
    return_frames(caller->krnl->mram, frames);
    return result;
  }

  while (frames != NULL)
  {
    struct framephy_struct *next = frames->fp_next;
    free(frames);
    frames = next;
  }
  return 0;
}

/* Swap copy content page from source frame to destination frame
 * @mpsrc  : source memphy
 * @srcfpn : source physical page number (FPN)
 * @mpdst  : destination memphy
 * @dstfpn : destination physical page number (FPN)
 **/
int __swap_cp_page(struct memphy_struct *mpsrc, addr_t srcfpn,
                   struct memphy_struct *mpdst, addr_t dstfpn)
{
  if (mpsrc == NULL || mpdst == NULL)
    return -1;

  for (int cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addr_t addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addr_t addrdst = dstfpn * PAGING64_PAGESZ + cellidx;
    BYTE data = 0;

    if (MEMPHY_read(mpsrc, addrsrc, &data) != 0 ||
        MEMPHY_write(mpdst, addrdst, data) != 0)
      return -1;
  }

  return 0;
}

/*
 *Initialize a empty Memory Management instance
 * @mm:     self mm
 * @caller: mm owner
 */
int init_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  (void)caller;
  if (mm == NULL)
    return -1;

  *mm = (struct mm_struct){0};

  addr_t *pgd = calloc(PAGE_TABLE_ENTRIES, sizeof(*pgd));
  struct vm_area_struct *vma = calloc(1, sizeof(*vma));
  if (pgd == NULL || vma == NULL)
  {
    free(pgd);
    free(vma);
    return -1;
  }

  vma->vm_mm = mm;
  mm->mmap = vma;
  mm->pgd = pgd;
  mm->page_table_bytes = PAGE_TABLE_ENTRIES * sizeof(*pgd);
  return 0;
}

/* Call after reclaiming RAM/swap frames and draining users of this mm. */
void destroy_mm(struct mm_struct *mm)
{
  if (mm == NULL)
    return;

  pthread_mutex_lock(&page_table_lock);
  if (mm->pgd != NULL)
    free_page_tables(mm->pgd, PAGE_TABLE_LEVELS);
  while (mm->mmap != NULL)
  {
    struct vm_area_struct *area = mm->mmap;
    mm->mmap = area->vm_next;
    while (area->vm_freerg_list != NULL)
    {
      struct vm_rg_struct *region = area->vm_freerg_list;
      area->vm_freerg_list = region->rg_next;
      free(region);
    }
    free(area);
  }
  while (mm->fifo_pgn != NULL)
  {
    struct pgn_t *node = mm->fifo_pgn;
    mm->fifo_pgn = node->pg_next;
    free(node);
  }
  free(mm->kcpooltbl);
  *mm = (struct mm_struct){0};
  pthread_mutex_unlock(&page_table_lock);
}

struct vm_rg_struct *init_vm_rg(addr_t rg_start, addr_t rg_end)
{
  struct vm_rg_struct *rgnode = malloc(sizeof(struct vm_rg_struct));
  if (rgnode == NULL)
    return NULL;

  rgnode->vmaid = 0;
  rgnode->rg_start = rg_start;
  rgnode->rg_end = rg_end;
  rgnode->rg_next = NULL;

  return rgnode;
}

int enlist_vm_rg_node(struct vm_rg_struct **rglist, struct vm_rg_struct *rgnode)
{
  if (rglist == NULL || rgnode == NULL)
    return -1;

  rgnode->rg_next = *rglist;
  *rglist = rgnode;

  return 0;
}

int enlist_pgn_node(struct pgn_t **plist, addr_t pgn)
{
  if (plist == NULL)
    return -1;

  struct pgn_t *pnode = malloc(sizeof(struct pgn_t));
  if (pnode == NULL)
    return -1;

  pnode->pgn = pgn;
  pnode->pg_next = *plist;
  *plist = pnode;

  return 0;
}

int print_list_fp(struct framephy_struct *ifp)
{
  struct framephy_struct *fp = ifp;

  printf("print_list_fp: ");
  if (fp == NULL) { printf("NULL list\n"); return -1;}
  printf("\n");
  while (fp != NULL)
  {
    printf("fp[" FORMAT_ADDR "]\n", fp->fpn);
    fp = fp->fp_next;
  }
  printf("\n");
  return 0;
}

int print_list_rg(struct vm_rg_struct *irg)
{
  struct vm_rg_struct *rg = irg;

  printf("print_list_rg: ");
  if (rg == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (rg != NULL)
  {
    printf("rg[" FORMAT_ADDR "->"  FORMAT_ADDR "]\n", rg->rg_start, rg->rg_end);
    rg = rg->rg_next;
  }
  printf("\n");
  return 0;
}

int print_list_vma(struct vm_area_struct *ivma)
{
  struct vm_area_struct *vma = ivma;

  printf("print_list_vma: ");
  if (vma == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (vma != NULL)
  {
    printf("va[" FORMAT_ADDR "->" FORMAT_ADDR "]\n", vma->vm_start, vma->vm_end);
    vma = vma->vm_next;
  }
  printf("\n");
  return 0;
}

int print_list_pgn(struct pgn_t *ip)
{
  printf("print_list_pgn: ");
  if (ip == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (ip != NULL)
  {
    printf("va[" FORMAT_ADDR "]-\n", ip->pgn);
    ip = ip->pg_next;
  }
  printf("\n");
  return 0;
}

/* Visit populated directory paths intersecting the requested page range. */
static void print_page_table_range(addr_t *table, unsigned int level, addr_t base,
                                   addr_t first, addr_t last, addr_t **path)
{
  path[level] = table;
  addr_t stride = BIT_ULL((PAGE_TABLE_LEVELS - 1 - level) * PAGE_TABLE_INDEX_BITS);

  for (unsigned int i = 0; i < PAGE_TABLE_ENTRIES; ++i)
  {
    addr_t entry_base = base + i * stride;
    if (entry_base > last)
      break;

    if (table[i] == 0 || entry_base + stride <= first)
      continue;

    if (level == PAGE_TABLE_LEVELS - 1)
    {
      printf(" PDG=%16p P4D=%16p PUD=%16p PMD=%16pPT=%16p\n",
             (void *)path[0], (void *)path[1], (void *)path[2],
             (void *)path[3], (void *)path[4]);
      return;
    }

    print_page_table_range((addr_t *)(uintptr_t)table[i], level + 1,
                           entry_base, first, last, path);
  }
}

int print_pgtbl(struct pcb_t *caller, addr_t start, addr_t end)
{
  const addr_t pgn_mask = BIT_ULL(PAGING64_CPU_BUS_WIDTH - PAGING64_ADDR_PT_SHIFT) - 1;
  addr_t *path[PAGE_TABLE_LEVELS];

  if (caller == NULL || caller->mm == NULL || start > end ||
      !is_canonical_address(start))
    return -1;

  /* Current callers use UINT64_MAX to dump all mapped directories. */
  addr_t last_addr = end == UINT64_MAX || end == start ? end : end - 1;
  if (!is_canonical_address(last_addr))
    return -1;

  struct mm_struct *mm = caller->mm;
  pthread_mutex_lock(&page_table_lock);
  if (mm->pgd != NULL && start != end)
    print_page_table_range(mm->pgd, 0, 0, (start >> PAGING64_ADDR_PT_SHIFT) & pgn_mask,
                           (last_addr >> PAGING64_ADDR_PT_SHIFT) & pgn_mask, path);

#ifdef MMDBG
  printf(" MMU pte_lookup_reads=%" PRIu64 " page_table_bytes=%" PRIu64 "\n",
         mm->pte_lookup_reads, mm->page_table_bytes);
#endif
  pthread_mutex_unlock(&page_table_lock);
  return 0;
}

#endif  //def MM64
