/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "common.h"
#include "libmem.h"
#include "mm.h"
#include "queue.h"
#include "syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct pcb_t *killall_find_caller(struct krnl_t *krnl, uint32_t pid)
{
   if (krnl == NULL || krnl->running_list == NULL)
      return NULL;

   struct queue_t *rlist = krnl->running_list;
   for (int attempt = 0; attempt < 2; attempt++)
   {
      int sz = rlist->size;
      if (sz > MAX_QUEUE_SIZE)
         sz = MAX_QUEUE_SIZE;
      for (int i = sz - 1; i >= 0; i--)
      {
         struct pcb_t *p = rlist->proc[i];
         if (p != NULL && p->pid == pid)
            return p;
      }
   }

   return NULL;
}

static int killall_match_name(const struct pcb_t *proc, const char *target_name)
{
   if (proc == NULL || target_name == NULL || target_name[0] == '\0')
      return 0;

   if (strcmp(proc->path, target_name) == 0)
      return 1;

   const char *slash = strrchr(proc->path, '/');
   const char *base = (slash != NULL) ? (slash + 1) : proc->path;
   return (strcmp(base, target_name) == 0);
}

static void killall_scan_queue(struct queue_t *q, const char *proc_name,
                               int remove_from_queue)
{
   if (q == NULL || q->size <= 0)
      return;

   for (int i = q->size - 1; i >= 0; i--)
   {
      struct pcb_t *proc = q->proc[i];
      if (proc != NULL && killall_match_name(proc, proc_name))
      {
         /* Signal process completion safely so CPU/scheduler terminates it */
         if (proc->code != NULL)
            proc->pc = proc->code->size;

         if (remove_from_queue)
            purgequeue(q, proc);
      }
   }
}

/*
 * __sys_killall - Syscall 101: terminate all processes matching the name
 * stored in caller's memory region @regs->a1 (terminated by -1 or '\0').
 */
int __sys_killall(struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs)
{
   char proc_name[100];
   uint32_t memrg;
   int i = 0;

   if (krnl == NULL || regs == NULL)
      return -1;

   struct pcb_t *caller = killall_find_caller(krnl, pid);
   if (caller == NULL)
      return -1;

   memrg = (uint32_t)regs->a1;
   memset(proc_name, 0, sizeof(proc_name));

   /* Read target process name from caller's user-space memory region */
   while (i < (int)(sizeof(proc_name) - 1))
   {
      BYTE ch = 0;
      if (__read(caller, 0, (int)memrg, (addr_t)i, &ch) != 0)
         break;

      if (ch == (BYTE)-1 || ch == '\0')
         break;

      proc_name[i++] = (char)ch;
   }
   proc_name[i] = '\0';

   if (proc_name[0] == '\0')
      return 0;

   /* Mark matching running processes for safe termination on their CPU */
   killall_scan_queue(krnl->running_list, proc_name, 0);

   /* Terminate and purge matching queued processes */
#ifdef MLQ_SCHED
   if (krnl->mlq_ready_queue != NULL)
   {
      for (int prio = 0; prio < MAX_PRIO; prio++)
         killall_scan_queue(&krnl->mlq_ready_queue[prio], proc_name, 1);
   }
#endif
   killall_scan_queue(krnl->ready_queue, proc_name, 1);

   return 0;
}
