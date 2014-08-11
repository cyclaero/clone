//  utils.c
//  clone
//
//  Created by Dr. Rolf Jansen on 2013-01-13.
//  Copyright (c) 2013-2014. All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without modification,
//  are permitted provided that the following conditions are met:
//
//  1. Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//  2. Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
//  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
//  AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
//  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
//  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
//  THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/acl.h>

#include "utils.h"


#pragma mark ••• Setting Times/Flags and Copying Extended Meta Data - EXAs & ACLs •••

extern int *gSourceFSType;
extern int *gDestinFSType;


#if defined (__APPLE__)

   #include <sys/xattr.h>

   void setTimesFlags(const char *dst, struct stat *st)
   {
      // On Mac OS X, set the flags before the times
      lchflags(dst, st->st_flags);

      struct timeval tv[2];
      tv[0].tv_sec = st->st_atimespec.tv_sec, tv[0].tv_usec = (int)(st->st_atimespec.tv_nsec/1000);
      if (st->st_birthtimespec.tv_sec != -1)
      {
         tv[1].tv_sec = st->st_birthtimespec.tv_sec, tv[1].tv_usec = (int)(st->st_birthtimespec.tv_nsec/1000);
         lutimes(dst, tv);
      }
      tv[1].tv_sec = st->st_mtimespec.tv_sec, tv[1].tv_usec = (int)(st->st_mtimespec.tv_nsec/1000);
      lutimes(dst, tv);
   }

   void getMetaData(int fd, const char *src, struct stat *st, ExtMetaData *xmd)
   {
      // Reading extended attributes
      exa_t   xa0 = NULL, *xan = &xa0, xa;
      ssize_t listsize = (fd != -1) ? flistxattr(fd, NULL, 0, XATTR_NOFOLLOW|XATTR_SHOWCOMPRESSION)
                                    : listxattr(src, NULL, 0, XATTR_NOFOLLOW|XATTR_SHOWCOMPRESSION);
      if (listsize > 0)
      {
         char *namelist = malloc(listsize);
         if (fd != -1)
            flistxattr(fd, namelist, listsize, XATTR_NOFOLLOW|XATTR_SHOWCOMPRESSION);
         else
            listxattr(src, namelist, listsize, XATTR_NOFOLLOW|XATTR_SHOWCOMPRESSION);

         char   *name = namelist;
         char   *value;
         ssize_t len, size;

         do
         {
            if (fd != -1)
               if ((size = fgetxattr(fd, name, NULL, 0, 0, XATTR_NOFOLLOW|XATTR_SHOWCOMPRESSION)) > 0)
                  fgetxattr(fd, name, value = malloc(size), size, 0, XATTR_NOFOLLOW|XATTR_SHOWCOMPRESSION);
               else
                  value = NULL;
            else
               if ((size = getxattr(src, name, NULL, 0, 0, XATTR_NOFOLLOW|XATTR_SHOWCOMPRESSION)) > 0)
                  getxattr(src, name, value = malloc(size), size, 0, XATTR_NOFOLLOW|XATTR_SHOWCOMPRESSION);
               else
                  value = NULL;

            xa = *xan = malloc(sizeof(ExtAttrs));
            xa->name  = name;
            xa->value = value;
            xa->size  = size;
            xa->next  = NULL;
            xan = &xa->next;

            len   = strlen(name) + 1;
            name += len;
         } while (listsize -= len);
      }
      xmd->exa = xa0;

      // Reading the ACLs
      xmd->acl = (fd != -1) ? acl_get_fd_np(fd, ACL_TYPE_EXTENDED) : acl_get_link_np(src, ACL_TYPE_EXTENDED);
   }

   void setMetaData(int fd, const char *dst, ExtMetaData *xmd)
   {
      // Writing the EXAs
      if (xmd->exa)
      {
         exa_t xa = xmd->exa;
         char *namelist = xa->name;

         while (xa)
         {
            if (fd != -1)
               fsetxattr(fd, xa->name, xa->value, xa->size, 0, XATTR_NOFOLLOW);
            else
               setxattr(dst, xa->name, xa->value, xa->size, 0, XATTR_NOFOLLOW);
            free(xa->value);
            void *fxa = xa;
            xa = xa->next;
            free(fxa);
         }
         free(namelist);
      }

      // Writing the ACLs
      if (xmd->acl)
      {
         if (fd != -1)
            acl_set_fd_np(fd, xmd->acl, ACL_TYPE_EXTENDED);
         else
            acl_set_link_np(dst, ACL_TYPE_EXTENDED, xmd->acl);
         acl_free(xmd->acl);
      }
   }

   void freeMetaData(ExtMetaData *xmd)
   {
      // Releasing the EXAs
      if (xmd->exa)
      {
         exa_t xa = xmd->exa;
         char *namelist = xa->name;

         while (xa)
         {
            free(xa->value);
            void *fxa = xa;
            xa = xa->next;
            free(fxa);
         }
         free(namelist);
      }

      // Releasing the ACLs
      if (xmd->acl)
         acl_free(xmd->acl);
   }

#elif defined (__FreeBSD__)

   #include <sys/extattr.h>

   void setTimesFlags(const char *dst, struct stat *st)
   {
      // From man 2 utimes (FreeBSD 9.1):
      /* If times is non-NULL, it is assumed to point to an array of two timeval
         structures.  The access time is set to the value of the first element,
         and the modification time is set to the value of the second element.  For
         file systems that support file birth (creation) times (such as UFS2), the
         birth time will be set to the value of the second element if the second
         element is older than the currently set birth time.  To set both a birth
         time and a modification time, two calls are required; the first to set
         the birth time and the second to set the (presumably newer) modification
         time.  Ideally a new system call will be added that allows the setting of
         all three times at once.  The caller must be the owner of the file or be
         the super-user. */

      struct timeval tv[2];
      tv[0].tv_sec = st->st_atimespec.tv_sec, tv[0].tv_usec = (int)(st->st_atimespec.tv_nsec/1000);
      if (st->st_birthtimespec.tv_sec != -1)
      {
         tv[1].tv_sec = st->st_birthtimespec.tv_sec, tv[1].tv_usec = (int)(st->st_birthtimespec.tv_nsec/1000);
         lutimes(dst, tv);
      }
      tv[1].tv_sec = st->st_mtimespec.tv_sec, tv[1].tv_usec = (int)(st->st_mtimespec.tv_nsec/1000);
      lutimes(dst, tv);

      // On FreeBSD, set the flags after the times
      lchflags(dst, st->st_flags);
   }

   void getMetaData(int fd, const char *src, struct stat *st, ExtMetaData *xmd)
   {
      // Reading the EXAs
      // FreeBSD got two name spaces for extended attributes
      // here we define system = 0 and user = 1)
      for (int i = 0; i < 2; i++)
      {
         int     ans = (i == 0) ? EXTATTR_NAMESPACE_SYSTEM : EXTATTR_NAMESPACE_USER;
         exa_t   xa0 = NULL, *xan = &xa0, xa;
         ssize_t listsize = ((fd != -1) ? extattr_list_fd(fd, ans, NULL, 0)
                                        : extattr_list_link(src, ans, NULL, 0)) + 1; // the extracted list is not nul terminated, so leave space for the extra nul
         if (listsize > 1)
         {
            // FIXME: occasionally, something in the system is writing beyond the supplied memory buffer.
            //        for the time being supply quite a lot space, 10times the indicated seems to work.
            char   *namelist = malloc(listsize *= 10);
            listsize = extattr_list_link(src, ans, namelist, listsize);

            // the name list is actually a sequence of joined pascal strings -> unsigned char
            uchar  *name = (uchar *)namelist;
            uchar   l0, l1 = *name++;

            char   *value;
            ssize_t size;

            do
            {
               l0 = l1, l1 = name[l0], name[l0] = '\0';

               if (fd != -1)
                  if ((size = extattr_get_fd(fd, ans, (char *)name, NULL, 0)) > 0)
                     extattr_get_fd(fd, ans, (char *)name, value = malloc(size), size);
                  else
                     value = NULL;
               else
                  if ((size = extattr_get_link(src, ans, (char *)name, NULL, 0)) > 0)
                     extattr_get_link(src, ans, (char *)name, value = malloc(size), size);
                  else
                     value = NULL;

               xa = *xan = malloc(sizeof(ExtAttrs));
               xa->name  = (char *)name;
               xa->value = value;
               xa->size  = size;
               xa->next  = NULL;
               xan = &xa->next;

               name += ++l0;
            } while ((listsize -= l0) > 0);
         }

         xmd->exa[i] = xa0;
      }

      // Reading the ACLs
      if (*gSourceFSType == *(int *)"ufs" && *gDestinFSType == *(int *)"ufs")
         // In the case of UFS2 file systems, the ACLs have been read already
         // as part of the extended attributes within the system namespace,
         // and therefore it is not necessary to read them again.
         xmd->acl[0] = xmd->acl[1] = NULL;
      else
      {
         acl_t acl;
         int trivial;

         if (fd != -1)
         {
            switch (fpathconf(fd, _PC_ACL_NFS4))
            {
               case 0:
                  acl = acl_get_fd_np(fd, ACL_TYPE_ACCESS);
                  break;

               case 1:
                  acl = acl_get_fd_np(fd, ACL_TYPE_NFS4);
                  break;

               default:
                  acl = NULL;
                  break;
            }

            if (acl && (acl_is_trivial_np(acl, &trivial) || trivial))
            {
               acl_free(acl);
               acl = NULL;
            }
            xmd->acl[0] = acl;
            xmd->acl[1] = NULL;
         }

         else // no open file discriptor
         {
            if ((acl = acl_get_link_np(src, ACL_TYPE_ACCESS)) &&
                (acl_is_trivial_np(acl, &trivial) || trivial))
            {
               acl_free(acl);
               acl = NULL;
            }
            xmd->acl[0] = acl;

            if (!S_ISDIR(st->st_mode))
               xmd->acl[1] = NULL;
            else
            {
               if ((acl = acl_get_link_np(src, ACL_TYPE_DEFAULT)) &&
                   ((uint*)acl)[1] == 0)
               {
                  acl_free(acl);
                  acl = NULL;
               }
               xmd->acl[1] = acl;
            }
         }
      }
   }

   void setMetaData(int fd, const char *dst, ExtMetaData *xmd)
   {
      // Writing the EXAs
      // FreeBSD got two name spaces for extended attributes
      // here we define system = 0 and user = 1)
      for (int i = 0; i < 2; i++)
         if (xmd->exa[i])
         {
            int   ans = (i == 0) ? EXTATTR_NAMESPACE_SYSTEM : EXTATTR_NAMESPACE_USER;
            exa_t xa = xmd->exa[i];
            char *namelist = xa->name;

            while (xa)
            {
               if (fd != -1)
                  extattr_set_fd(fd, ans, xa->name, xa->value, xa->size);
               else
                  extattr_set_link(dst, ans, xa->name, xa->value, xa->size);
               free(xa->value);
               void *fxa = xa;
               xa = xa->next;
               free(fxa);
            }
            free(namelist);
         }

      // Writing the ACLs
      if (xmd->acl[0])
      {
         if (fd != -1)
            acl_set_fd_np(fd, xmd->acl[0], ACL_TYPE_ACCESS);
         else
            acl_set_link_np(dst, ACL_TYPE_ACCESS, xmd->acl[0]);
         acl_free(xmd->acl[0]);
      }

      if (xmd->acl[1])
      {  // only directories (no open fd) may have default ACLs
         acl_set_link_np(dst, ACL_TYPE_DEFAULT, xmd->acl[1]);
         acl_free(xmd->acl[1]);
      }
   }

   void freeMetaData(ExtMetaData *xmd)
   {
      // Releasing the EXAs
      for (int i = 0; i < 2; i++)
         if (xmd->exa[i])
         {
            exa_t xa = xmd->exa[i];
            char *namelist = xa->name;

            while (xa)
            {
               free(xa->value);
               void *fxa = xa;
               xa = xa->next;
               free(fxa);
            }
            free(namelist);
         }

      // Releasing the ACLs
      if (xmd->acl[0])
         acl_free(xmd->acl[0]);

      if (xmd->acl[1])
         acl_free(xmd->acl[1]);
   }

#endif


#pragma mark ••• AVL Tree •••

#pragma mark ••• Value Data householding •••

void releaseValue(Value *value)
{
   switch (-value->kind)   // dynamic data, has to be released
   {
      case Simple:
      case Data:
         free(value->v.p);
         break;

      case String:
         free(value->v.s);
         break;

      case Dictionary:
         releaseTable((Node **)value->v.p);
         break;
   }
}


static int balanceNode(Node **node)
{
   int   change = 0;
   Node *o = *node;
   Node *p, *q;

   if (o->B == -2)
   {
      if (p = o->L)                    // make the static analyzer happy
         if (p->B == +1)
         {
            change = 1;                // double left-right rotation
            q      = p->R;             // left rotation
            p->R   = q->L;
            q->L   = p;
            o->L   = q->R;             // right rotation
            q->R   = o;
            o->B   = +(q->B < 0);
            p->B   = -(q->B > 0);
            q->B   = 0;
            *node  = q;
         }

         else
         {
            change = p->B;             // single right rotation
            o->L   = p->R;
            p->R   = o;
            o->B   = -(++p->B);
            *node  = p;
         }
   }

   else if (o->B == +2)
   {
      if (q = o->R)                    // make the static analyzer happy
         if (q->B == -1)
         {
            change = 1;                // double right-left rotation
            p      = q->L;             // right rotation
            q->L   = p->R;
            p->R   = q;
            o->R   = p->L;             // left rotation
            p->L   = o;
            o->B   = -(p->B > 0);
            q->B   = +(p->B < 0);
            p->B   = 0;
            *node  = p;
         }

         else
         {
            change = q->B;             // single left rotation
            o->R   = q->L;
            q->L   = o;
            o->B   = -(--q->B);
            *node  = q;
         }
   }

   return change != 0;
}


static int pickPrevNode(Node **node, Node **exch)
{                                       // *exch on entry = parent node
   Node *o = *node;                     // *exch on exit  = picked previous value node

   if (o->R)
   {
      *exch = o;
      int change = -pickPrevNode(&o->R, exch);
      if (change)
         if (abs(o->B += change) > 1)
            return balanceNode(node);
         else
            return o->B == 0;
      else
         return 0;
   }

   else if (o->L)
   {
      Node *p = o->L;
      o->L = NULL;
      (*exch)->R = p;
      *exch = o;
      return p->B == 0;
   }

   else
   {
      (*exch)->R = NULL;
      *exch = o;
      return 1;
   }
}


static int pickNextNode(Node **node, Node **exch)
{                                       // *exch on entry = parent node
   Node *o = *node;                     // *exch on exit  = picked next value node

   if (o->L)
   {
      *exch = o;
      int change = +pickNextNode(&o->L, exch);
      if (change)
         if (abs(o->B += change) > 1)
            return balanceNode(node);
         else
            return o->B == 0;
      else
         return 0;
   }

   else if (o->R)
   {
      Node *q = o->R;
      o->R = NULL;
      (*exch)->L = q;
      *exch = o;
      return q->B == 0;
   }

   else
   {
      (*exch)->L = NULL;
      *exch = o;
      return 1;
   }
}


// CAUTION: It is an error to call these functions with key being 0 AND name being NULL.
//          Either of both must be non-zero. No error cheking is done within these recursive functions.

static inline llong order(ullong key, const char *name, Node *node)
{
   llong result;

   if (key)
      if ((result = key - node->key) || !name)
         return result;

   return strcmp(name, node->name);
}

Node *findTreeNode(ullong key, const char *name, Node *node)
{
   if (node)
   {
      llong ord = order(key, name, node);

      if (ord == 0)
         return node;

      else if (ord < 0)
         return findTreeNode(key, name, node->L);

      else // (ord > 0)
         return findTreeNode(key, name, node->R);
   }

   else
      return NULL;
}

int addTreeNode(ullong key, const char *name, ssize_t naml, Value *value, Node **node, Node **passed)
{
   static const Value zeros = {0, {.i = 0}};

   Node *o = *node;

   if (o == NULL)                         // if the key is not in the tree
   {                                      // then add it into a new leaf
      o = *node = *passed = calloc(1, sizeof(Node));
      o->key = key;
      o->name = strcpy(malloc(naml+1), name);
      o->naml = naml;
      if (value)
         o->value = *value;
      return 1;                           // add the weight of 1 leaf onto the balance
   }

   else
   {
      int   change;
      llong ord = order(key, name, o);

      if (ord == 0)                       // if the name is already in the tree then
      {
         releaseValue(&o->value);         // release the old value - if kind is empty then releaseValue() does nothing
         o->value = (value) ? *value      // either store the new one or
                            :  zeros;     // zero-out the value struct
         *passed = o;                     // report back the node in which the value was modified
         return 0;
      }

      else if (ord < 0)
         change = -addTreeNode(key, name, naml, value, &o->L, passed);

      else // (ord > 0)
         change = +addTreeNode(key, name, naml, value, &o->R, passed);

      if (change)
         if (abs(o->B += change) > 1)
            return 1 - balanceNode(node);
         else
            return o->B != 0;
      else
         return 0;
   }
}

int removeTreeNode(ullong key, const char *name, Node **node)
{
   Node *o = *node;

   if (o == NULL)
      return 0;                              // not found -> recursively do nothing

   else
   {
      int   change;
      llong ord = order(key, name, o);

      if (ord == 0)
      {
         int    b = o->B;
         Node  *p = o->L;
         Node  *q = o->R;

         if (!p || !q)
         {
            free((*node)->name);
            releaseValue(&(*node)->value);
            free(*node);
            *node = (p > q) ? p : q;
            return 1;                        // remove the weight of 1 leaf from the balance
         }

         else
         {
            if (b == -1)
            {
               if (!p->R)
               {
                  change = +1;
                  o      =  p;
                  o->R   =  q;
               }
               else
               {
                  change = +pickPrevNode(&p, &o);
                  o->L   =  p;
                  o->R   =  q;
               }
            }

            else
            {
               if (!q->L)
               {
                  change = -1;
                  o      =  q;
                  o->L   =  p;
               }
               else
               {
                  change = -pickNextNode(&q, &o);
                  o->L   =  p;
                  o->R   =  q;
               }
            }

            o->B = b;
            free((*node)->name);
            releaseValue(&(*node)->value);
            free(*node);
            *node = o;
         }
      }

      else if (ord < 0)
         change = +removeTreeNode(key, name, &o->L);

      else // (ord > 0)
         change = -removeTreeNode(key, name, &o->R);

      if (change)
         if (abs(o->B += change) > 1)
            return balanceNode(node);
         else
            return o->B == 0;
      else
         return 0;
   }
}

void releaseTree(Node *node)
{
   if (node)
   {
      if (node->L)
         releaseTree(node->L);
      if (node->R)
         releaseTree(node->R);

      free(node->name);
      releaseValue(&node->value);
      free(node);
   }
}


#pragma mark ••• Hash Table •••

// Essence of MurmurHash3_x86_32()
//
//  Original at: http://code.google.com/p/smhasher/source/browse/trunk/MurmurHash3.cpp
//
//  Quote from the Source:
//  "MurmurHash3 was written by Austin Appleby, and is placed in the public
//   domain. The author hereby disclaims copyright to this source code."
//
// Many thanks to Austin!

static inline uint mmh3(const char *name, ssize_t naml)
{
   int    i, n   = (int)(naml/4);
   uint   k1, h1 = (uint)naml;          // quite tiny (0.2 %) better distribution by seeding the name length
   uint  *quads  = (uint *)(name + n*4);
   uchar *tail   = (uchar *)quads;

   for (i = -n; i; i++)
   {
      k1  = quads[i];
      k1 *= 0xCC9E2D51;
      k1  = (k1<<15)|(k1>>17);
      k1 *= 0x1B873593;

      h1 ^= k1;
      h1  = (h1<<13)|(h1>>19);
      h1  = h1*5 + 0xE6546B64;
   }

   k1 = 0;
   switch (naml & 3)
   {
      case 3: k1 ^= (uint)(tail[2] << 16);
      case 2: k1 ^= (uint)(tail[1] << 8);
      case 1: k1 ^= (uint)(tail[0]);
              k1 *= 0xCC9E2D51;
              k1  = (k1<<15)|(k1>>17);
              k1 *= 0x1B873593;
              h1 ^= k1;
   };

   h1 ^= naml;
   h1 ^= h1 >> 16;
   h1 *= 0x85EBCA6B;
   h1 ^= h1 >> 13;
   h1 *= 0xC2B2AE35;
   h1 ^= h1 >> 16;

   return h1;
}


// Table creation and release
Node **createTable(uint n)
{
   Node **table = calloc(n + 1, sizeof(Node *));
   *(uint *)table = n;
   return table;
}

void releaseTable(Node *table[])
{
   if (table)
   {
      uint i, n = *(uint *)table;
      for (i = 1; i <= n; i++)
         releaseTree(table[i]);
      free(table);
   }
}


// The key-name/value store serves for two purposes.
//
//   1. quickly store/find inodes (unsigned long values)
//      and the respective source file/link name
//
//   2. store and retrieve file system names that
//      exist in the destination hierarchy.
//
// Inodes of the files/links are sufficiently random for direct
// use, without passing them through a hash function, e.g.:
//
//    table[key%n + 1]
//
// File system names must be hashed before:
//
//    table[mmh3(fsname)%n + 1]
//
// For the sake of efficiency and clarity of usage here come
// two sets of find...(), store...(), and remove...() functions.


// Storing/retrieving file system names by inode
Node *findINode(Node *table[], ullong key)
{
   uint n = *(uint *)table;
   return findTreeNode(key, NULL, table[key%n + 1]);
}

Node *storeINode(Node *table[], ullong key, const char *fsname, ssize_t naml, long dev)
{
   if (fsname && *fsname)
   {
      if (naml < 0)
         naml = strlen(fsname);
      uint  n = *(uint *)table;
      Value value = {Simple, {.i = dev}};
      Node *passed;
      addTreeNode(key, fsname, naml, &value, &table[key%n + 1], &passed);
      return passed;
   }
   else
      return NULL;
}

void removeINode(Node *table[], ullong key)
{
   uint  tidx = key % *(uint *)table + 1;
   Node *node = table[tidx];
   if (node)
      if (!node->L && !node->R)
   {
      free(node->name);
      releaseValue(&node->value);
      free(node);
      table[tidx] = NULL;
   }
   else
      removeTreeNode(key, node->name, &table[tidx]);
}


// Storing/retrieving file system names
Node *findFSName(Node *table[], const char *fsname, ssize_t naml)
{
   if (fsname && *fsname)
   {
      if (naml < 0)
         naml = strlen(fsname);
      uint  n = *(uint *)table;
      ulong hkey = mmh3(fsname, naml);
      return findTreeNode(hkey, fsname, table[hkey%n + 1]);
   }
   else
      return NULL;
}

Node *storeFSName(Node *table[], const char *fsname, ssize_t naml, Value *value)
{
   if (fsname && *fsname)
   {
      if (naml < 0)
         naml = strlen(fsname);
      uint  n = *(uint *)table;
      ulong hkey = mmh3(fsname, naml);
      Node *passed;
      addTreeNode(hkey, fsname, naml, value, &table[hkey%n + 1], &passed);
      return passed;
   }
   else
      return NULL;
}

void removeFSName(Node *table[], const char *fsname, ssize_t naml)
{
   if (fsname && *fsname)
   {
      if (naml < 0)
         naml = strlen(fsname);
      ulong hkey = mmh3(fsname, naml);
      uint  tidx = hkey % *(uint *)table + 1;
      Node *node = table[tidx];
      if (node)
         if (!node->L && !node->R)
      {
         free(node->name);
         releaseValue(&node->value);
         free(node);
         table[tidx] = NULL;
      }
      else
         removeTreeNode(hkey, fsname, &table[tidx]);
   }
}
