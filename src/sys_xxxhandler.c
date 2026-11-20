/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "common.h"
#include "syscall.h"
#include <inttypes.h>
#include <stdio.h>

/*
 * __sys_xxxhandler - Syscall 440 custom handler (per Section 1.6.3 of spec)
 */
int __sys_xxxhandler(struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs)
{
   (void)krnl;
   (void)pid;

   if (regs == NULL)
      return -1;

   printf("The first system call parameter %" PRIu64 "\n", (uint64_t)regs->a1);
   return 0;
}
