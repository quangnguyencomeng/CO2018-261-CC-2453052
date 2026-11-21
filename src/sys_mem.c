/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "os-mm.h"
#include "syscall.h"
#include "libmem.h"
#include "sched.h"
#include <stdlib.h>

#ifdef MM64
#include "mm64.h"
#else
#include "mm.h"
#endif

int __sys_memmap(struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs)
{
   if (krnl == NULL || regs == NULL)
      return -1;

   int memop = (int)regs->a1;
   BYTE value = 0;
   int ret = 0;

   /*
    * @bksysnet: Please note in the dual spacing design
    *            syscall implementations are in kernel space.
    *            User processes pass PID, and kernel resolves PCB via running_list.
    */
   struct pcb_t *caller = find_proc_by_pid(pid);

   switch (memop)
   {
   case SYSMEM_MAP_OP:
      /* Reserved process case */
      if (caller == NULL)
         return -1;
      ret = vmap_pgd_memset(caller, regs->a2, (int)regs->a3);
      break;
   case SYSMEM_INC_OP:
      if (caller == NULL)
         return -1;
      ret = inc_vma_limit(caller, (int)regs->a2, regs->a3);
      break;
   case SYSMEM_SWP_OP:
      if (caller == NULL)
         return -1;
      ret = __mm_swap_page(caller, regs->a2, regs->a3);
      break;
   case SYSMEM_IO_READ:
      ret = MEMPHY_read(krnl->mram, regs->a2, &value);
      regs->a3 = (arg_t)(unsigned char)value;
      break;
   case SYSMEM_IO_WRITE:
      ret = MEMPHY_write(krnl->mram, regs->a2, (BYTE)regs->a3);
      break;
   default:
      printf("Memop code: %d\n", memop);
      ret = -1;
      break;
   }

   return ret;
}
