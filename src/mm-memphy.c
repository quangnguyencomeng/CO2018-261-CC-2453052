/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

// #ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Memory physical module mm/mm-memphy.c
 */

#include "mm.h"
#include "mm64.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t memphy_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 *  MEMPHY_mv_csr - move MEMPHY cursor
 *  @mp: memphy struct
 *  @offset: offset
 */
int MEMPHY_mv_csr(struct memphy_struct *mp, addr_t offset)
{
   int numstep = 0;

   if (mp == NULL || mp->maxsz <= 0)
      return -1;

   mp->cursor = 0;
   while (numstep < (int)offset && numstep < mp->maxsz)
   {
      /* Traverse sequentially */
      mp->cursor = (mp->cursor + 1) % mp->maxsz;
      numstep++;
   }

   return 0;
}

/*
 * Internal unlocked sequential read/write helpers (called with memphy_lock held)
 */
static int memphy_seq_read_unlocked(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL || mp->storage == NULL || value == NULL ||
       mp->maxsz <= 0 || addr >= (addr_t)mp->maxsz)
      return -1;

   if (mp->rdmflg)
      return -1; /* Not compatible mode for sequential read */

   MEMPHY_mv_csr(mp, addr);
   *value = (BYTE)mp->storage[addr];

   return 0;
}

static int memphy_seq_write_unlocked(struct memphy_struct *mp, addr_t addr, BYTE value)
{
   if (mp == NULL || mp->storage == NULL ||
       mp->maxsz <= 0 || addr >= (addr_t)mp->maxsz)
      return -1;

   if (mp->rdmflg)
      return -1; /* Not compatible mode for sequential write */

   MEMPHY_mv_csr(mp, addr);
   mp->storage[addr] = value;

   return 0;
}

/*
 *  MEMPHY_seq_read - read MEMPHY device sequentially
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_seq_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   pthread_mutex_lock(&memphy_lock);
   int ret = memphy_seq_read_unlocked(mp, addr, value);
   pthread_mutex_unlock(&memphy_lock);
   return ret;
}

/*
 *  MEMPHY_read - read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL || mp->storage == NULL || value == NULL ||
       mp->maxsz <= 0 || addr >= (addr_t)mp->maxsz)
      return -1;

   pthread_mutex_lock(&memphy_lock);
   int ret = 0;
   if (mp->rdmflg)
      *value = mp->storage[addr];
   else /* Sequential access device */
      ret = memphy_seq_read_unlocked(mp, addr, value);
   pthread_mutex_unlock(&memphy_lock);

   return ret;
}

/*
 *  MEMPHY_seq_write - write MEMPHY device sequentially
 *  @mp: memphy struct
 *  @addr: address
 *  @value: written data
 */
int MEMPHY_seq_write(struct memphy_struct *mp, addr_t addr, BYTE value)
{
   pthread_mutex_lock(&memphy_lock);
   int ret = memphy_seq_write_unlocked(mp, addr, value);
   pthread_mutex_unlock(&memphy_lock);
   return ret;
}

/*
 *  MEMPHY_write - write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_write(struct memphy_struct *mp, addr_t addr, BYTE data)
{
   if (mp == NULL || mp->storage == NULL ||
       mp->maxsz <= 0 || addr >= (addr_t)mp->maxsz)
      return -1;

   pthread_mutex_lock(&memphy_lock);
   int ret = 0;
   if (mp->rdmflg)
      mp->storage[addr] = data;
   else /* Sequential access device */
      ret = memphy_seq_write_unlocked(mp, addr, data);
   pthread_mutex_unlock(&memphy_lock);

   return ret;
}

/*
 *  MEMPHY_format - format MEMPHY device into free frame list
 *  @mp: memphy struct
 *  @pagesz: page/frame size in bytes
 */
int MEMPHY_format(struct memphy_struct *mp, int pagesz)
{
   /* This setting comes with fixed constant PAGESZ */
   if (mp == NULL || pagesz <= 0)
      return -1;

   int numfp = mp->maxsz / pagesz;
   struct framephy_struct *newfst, *fst;
   int iter = 0;

   mp->free_fp_list = NULL;
   mp->used_fp_list = NULL;

   if (numfp <= 0)
      return -1;

   /* Init head of free framephy list */
   fst = (struct framephy_struct *)malloc(sizeof(struct framephy_struct));
   if (fst == NULL)
      return -1;
   fst->fpn = (addr_t)iter;
   fst->fp_next = NULL;
   fst->owner = NULL;
   mp->free_fp_list = fst;

   /* We have list with first element, fill in the rest num-1 elements */
   for (iter = 1; iter < numfp; iter++)
   {
      newfst = (struct framephy_struct *)malloc(sizeof(struct framephy_struct));
      if (newfst == NULL)
         return -1;
      newfst->fpn = (addr_t)iter;
      newfst->fp_next = NULL;
      newfst->owner = NULL;
      fst->fp_next = newfst;
      fst = newfst;
   }

   return 0;
}

/*
 *  MEMPHY_get_freefp - pop one free physical frame from free_fp_list
 *  @mp: memphy struct
 *  @retfpn: output frame page number
 */
int MEMPHY_get_freefp(struct memphy_struct *mp, addr_t *retfpn)
{
   if (mp == NULL || retfpn == NULL)
      return -1;

   pthread_mutex_lock(&memphy_lock);
   struct framephy_struct *fp = mp->free_fp_list;

   if (fp == NULL)
   {
      pthread_mutex_unlock(&memphy_lock);
      return -1;
   }

   *retfpn = fp->fpn;
   mp->free_fp_list = fp->fp_next;
   free(fp);

   pthread_mutex_unlock(&memphy_lock);
   return 0;
}

/*
 *  MEMPHY_dump - dump non-zero bytes in physical memory for tracing/IODUMP
 *  @mp: memphy struct
 */
int MEMPHY_dump(struct memphy_struct *mp)
{
   if (mp == NULL || mp->storage == NULL)
      return -1;

   pthread_mutex_lock(&memphy_lock);
   printf("===== PHYSICAL MEMORY DUMP =====\n");
   for (int i = 0; i < mp->maxsz; i++)
   {
      if (mp->storage[i] != 0)
      {
         printf("BYTE %08x: %d\n", i, (int)mp->storage[i]);
      }
   }
   printf("===== PHYSICAL MEMORY END-DUMP =====\n");
   pthread_mutex_unlock(&memphy_lock);

   return 0;
}

/*
 *  MEMPHY_put_freefp - return a physical frame number to free_fp_list
 *  @mp: memphy struct
 *  @fpn: frame page number to release
 */
int MEMPHY_put_freefp(struct memphy_struct *mp, addr_t fpn)
{
   if (mp == NULL)
      return -1;

   struct framephy_struct *newnode =
       (struct framephy_struct *)malloc(sizeof(struct framephy_struct));
   if (newnode == NULL)
      return -1;

   pthread_mutex_lock(&memphy_lock);
   newnode->fpn = fpn;
   newnode->owner = NULL;
   newnode->fp_next = mp->free_fp_list;
   mp->free_fp_list = newnode;
   pthread_mutex_unlock(&memphy_lock);

   return 0;
}

/*
 *  init_memphy - Initialize MEMPHY struct
 */
int init_memphy(struct memphy_struct *mp, addr_t max_size, int randomflg)
{
   if (mp == NULL)
      return -1;

   mp->maxsz = (int)max_size;
   mp->free_fp_list = NULL;
   mp->used_fp_list = NULL;
   mp->cursor = 0;

   if (max_size > 0)
   {
      mp->storage = (BYTE *)calloc((size_t)max_size, sizeof(BYTE));
      if (mp->storage == NULL)
         return -1;
   }
   else
   {
      mp->storage = NULL;
   }

#ifdef MM64
   MEMPHY_format(mp, PAGING64_PAGESZ);
#else
   MEMPHY_format(mp, PAGING_PAGESZ);
#endif

   mp->rdmflg = (randomflg != 0) ? 1 : 0;

   if (!mp->rdmflg) /* Not random access device, then serial device */
      mp->cursor = 0;

   return 0;
}

// #endif
