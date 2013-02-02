//  utils.c
//  clone
//
//  Created by Dr. Rolf Jansen on 2013-01-13.
//  Copyright (c) 2013 Cyclaero Ltda.
//  All rights reserved.
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


#pragma mark ••• Passing all Attribute •••

void setAttributes(const char *src, const char *dst, struct stat *st)
{
   struct timeval tv[2];
   ExtMetaData    xmd;

   lchown(dst, st->st_uid, st->st_gid);
   lchmod(dst, st->st_mode & ALLPERMS);

   getMetaData(src, &xmd);
   setMetaData(dst, &xmd);

   lutimes(dst, utimeset(st, tv));
   lchflags(dst, st->st_flags);
}


#pragma mark ••• Copying Extended Meta Data - EXAs & ACLs •••

#if defined (__APPLE__)

   #include <sys/xattr.h>

   void getMetaData(const char *src, ExtMetaData *xmd)
   {
      // reading extended attributes
      exa_t   xa0 = NULL, *xan = &xa0, xa;
      ssize_t listsize = listxattr(src, NULL, 0, XATTR_NOFOLLOW|XATTR_SHOWCOMPRESSION);

      if (listsize > 0)
      {
         char   *namelist = malloc(listsize);
                            listxattr(src, namelist, listsize, XATTR_NOFOLLOW|XATTR_SHOWCOMPRESSION);

         char   *name = namelist;
         char   *value;
         ssize_t len, size;

         do
         {
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

      xmd->exa[0] = xa0;

      // reading the ACLs
      xmd->acl[0] = acl_get_link_np(src, ACL_TYPE_EXTENDED);
   }

   void setMetaData(const char *dst, ExtMetaData *xmd)
   {
      // writing the EXAs
      if (xmd->exa[0])
      {
         exa_t xa = xmd->exa[0];
         char *namelist = xa->name;

         while (xa)
         {
            setxattr(dst, xa->name, xa->value, xa->size, 0, XATTR_NOFOLLOW);
            free(xa->value);
            void *fxa = xa;
            xa = xa->next;
            free(fxa);
         }
         free(namelist);
      }

      // writing the ACLs
      if (xmd->acl[0])
      {
         acl_set_link_np(dst, ACL_TYPE_EXTENDED, xmd->acl[0]);
         acl_free(xmd->acl[0]);
      }
   }

   void freeMetaData(ExtMetaData *xmd)
   {
      // releasing the EXAs
      if (xmd->exa[0])
      {
         exa_t xa = xmd->exa[0];
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

      // releasing the ACLs
      if (xmd->acl[0])
         acl_free(xmd->acl[0]);
   }

#elif defined (__FreeBSD__)

   #include <sys/extattr.h>

   void getMetaData(const char *src, ExtMetaData *xmd)
   {
      // reading the EXAs
      // FreeBSD got two name spaces for extended attributes
      // here we define system = 0 and user = 1)
      for (int i = 0; i < 2; i++)
      {
         int     ans = (i == 0) ? EXTATTR_NAMESPACE_SYSTEM : EXTATTR_NAMESPACE_USER;
         exa_t   xa0 = NULL, *xan = &xa0, xa;
         ssize_t listsize = extattr_list_link(src, ans, NULL, 0);
         if (listsize > 0)
         {                     // FIXME: occasionally, something is writing beyond the supplied memory buffer - for the time being supply sufficient space
            char   *namelist = malloc(listsize = MAX(listsize*10, 65536)); // the extracted list is not nul terminated, so leave space for the extra nul
                    listsize = extattr_list_link(src, ans, namelist, listsize);

            // the name list is actually a sequence of joined pascal strings -> unsigned char
            uchar  *name = (uchar *)namelist;
            uchar   l0, l1 = *name++;

            char   *value;
            ssize_t size;

            do
            {
               l0 = l1, l1 = name[l0], name[l0] = '\0';

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

      // reading the ACLs
      xmd->acl[0] = acl_get_link_np(src, ACL_TYPE_ACCESS);
      xmd->acl[1] = acl_get_link_np(src, ACL_TYPE_DEFAULT);
      xmd->acl[2] = acl_get_link_np(src, ACL_TYPE_NFS4);
   }

   void setMetaData(const char *dst, ExtMetaData *xmd)
   {
      // writing the EXAs
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
               extattr_set_link(dst, ans, xa->name, xa->value, xa->size);
               free(xa->value);
               void *fxa = xa;
               xa = xa->next;
               free(fxa);
            }
            free(namelist);
         }

      // writing the ACLs
      if (xmd->acl[0])
      {
         acl_set_link_np(dst, ACL_TYPE_ACCESS, xmd->acl[0]);
         acl_free(xmd->acl[0]);
      }

      if (xmd->acl[1])
      {
         acl_set_link_np(dst, ACL_TYPE_DEFAULT, xmd->acl[1]);
         acl_free(xmd->acl[1]);
      }

      if (xmd->acl[2])
      {
         acl_set_link_np(dst, ACL_TYPE_NFS4, xmd->acl[2]);
         acl_free(xmd->acl[2]);
      }
   }

   void freeMetaData(ExtMetaData *xmd)
   {
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

      // writing the ACLs
      if (xmd->acl[0])
         acl_free(xmd->acl[0]);

      if (xmd->acl[1])
         acl_free(xmd->acl[1]);

      if (xmd->acl[2])
         acl_free(xmd->acl[2]);
   }

#endif


#pragma mark ••• AVL Tree •••

static int balanceNode(Node **node)
{
   int   change = 0;
   Node *o = *node;
   Node *p, *q;

   if (o->B == -2)
   {
      p = o->L;                     // checked for !o->L in RemoveCalcNode()
      if (p->B == +1)               // so p would never be a NULL pointer
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
      q = o->R;                     // checked for !o->R in RemoveCalcNode()
      if (q->B == -1)               // so q would never be a NULL pointer
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
   int   change;                        // *exch on exit  = picked previous value node
   Node *o = *node;
   Node *p;

   if (o->R)
   {
      *exch = o;
      change = -pickPrevNode(&o->R, exch);
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
      p    = o->L;
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
   int   change;                        // *exch on exit  = picked next value node
   Node *o = *node;
   Node *q;

   if (o->L)
   {
      *exch = o;
      change = +pickNextNode(&o->L, exch);
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
      q    = o->R;
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


Node *findTreeNode(ulong key, Node *node)
{
   long ord;

   if (!node || (ord = key - node->key) == 0)
      return node;

   else if (ord < 0)
      return findTreeNode(key, node->L);

   else // (ord > 0)
      return findTreeNode(key, node->R);
}


int addTreeNode(ulong key, const char *name, int dev, Node **node, Node **passed)
{
   int   change = 0;
   long  ord;
   Node *o = *node;

   if (o == NULL)                                  // if the key is not in the tree
   {                                               // then add it into a new leaf
      o = *node = *passed = calloc(1, sizeof(Node));
      o->key  = key;
      o->name = strcpy(malloc(strlen(name)+1), name);
      o->dev  = dev;
      return 1;                                    // put the weight of 1 leaf onto the balance
   }

   else if ((ord = key - o->key) == 0)             // if the key is already in the tree then
   {
      *passed = o;                                 // report the passed node into which the new value has been entered
      free(o->name);                               // release the old memory allocation and
      o->name = strcpy(malloc(strlen(name)+1), name);
      o->dev  = dev;
      return 0;
   }

   else if (ord < 0)
      change = -addTreeNode(key, name, dev, &o->L, passed);

   else // (ord > 0)
      change = +addTreeNode(key, name, dev, &o->R, passed);

   if (change)
      if (abs(o->B += change) > 1)
         return 1 - balanceNode(node);
      else
         return o->B != 0;
   else
      return 0;
}


int removeTreeNode(ulong key, Node **node)
{
   int    change = 0;
   long   ord;
   Node  *o = *node;

   if (o == NULL)
      return 0;                                    // not found -> recursively do nothing

   else if ((ord = key - o->key) == 0)
   {
      int    b = o->B;
      Node  *p = o->L;
      Node  *q = o->R;

      if (!p || !q)
      {
         free((*node)->name);
         free(*node);
         *node = (p > q) ? p : q;
         return 1;                                // remove the weight of 1 leaf from the balance
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
         free(*node);
         *node = o;
      }
   }

   else if (ord < 0)
      change = +removeTreeNode(key, &o->L);

   else // (ord > 0)
      change = -removeTreeNode(key, &o->R);

   if (change)
      if (abs(o->B += change) > 1)
         return balanceNode(node);
      else
         return o->B == 0;
   else
      return 0;
}


void freeTree(Node *node)
{
   if (node)
   {
      if (node->L)
         freeTree(node->L);
      if (node->R)
         freeTree(node->R);

      free(node->name);
      free(node);
   }
}


#pragma mark ••• Hash Table •••

// Note: This implementation uses the inodes of the files/links
// as the keys. The inodes are sufficiently random for direct
// use, without passing through a hash function, i.e:
//
//    table[key%n + 1]
//
// instead of:
//
//    table[hash(key)%n + 1]


Node **createTable(uint n)
{
   Node **table = calloc(n + 1, sizeof(Node *));
   *(uint *)table = n;
   return table;
}

Node *findTableEntry(Node *table[], ulong key)
{
   uint n = *(uint *)table;
   return findTreeNode(key, table[key%n + 1]);
}

Node *addTableEntry(Node *table[], ulong key, const char *name, int dev)
{
   uint n = *(uint *)table;
   Node *passed;
   addTreeNode(key, name, dev, &table[key%n + 1], &passed);
   return passed;
}

void removeTableEntry(Node *table[], ulong key)
{
   uint n = *(uint *)table;
   removeTreeNode(key, &table[key%n + 1]);
}

void freeTable(Node *table[])
{
   if (table)
   {
      uint i, n = *(uint *)table;
      for (i = 1; i <= n; i++)
         freeTree(table[i]);
      free(table);
   }
}
