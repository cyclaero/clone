//  utils.h
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


#define  no_error  0
#define bad_error -1
#define sys_error -2

#define ENQUOTING(cvar) #cvar
#define STRINGIFY(cvar) ENQUOTING(cvar)


typedef unsigned char  uchar;
typedef unsigned short ushort;
typedef unsigned int   uint;
typedef unsigned long  ulong;

typedef long long          llong;
typedef unsigned long long ullong;


static inline struct timeval *utimeset(struct stat *st, struct timeval *tv)
{
   tv[0].tv_sec = st->st_atimespec.tv_sec, tv[0].tv_usec = (int)(st->st_atimespec.tv_nsec/1000);
   tv[1].tv_sec = st->st_mtimespec.tv_sec, tv[1].tv_usec = (int)(st->st_mtimespec.tv_nsec/1000);
   return tv;
}

void setAttributes(const char *src, const char *dst, struct stat *st);

#pragma mark ••• Copying extended Meta Data - EAs & ACLs •••

typedef struct ExtAttrs *exa_t;
typedef struct ExtAttrs
{
   char   *name;
   void   *value;
   ssize_t size;
   exa_t   next;
} ExtAttrs;


#if defined (__APPLE__)

   #define fnocache(fd) fcntl(fd, F_NOCACHE, 1)

   typedef struct
   {
      exa_t exa[1];  // Mac OS X got a single name space for all extended attributes
      acl_t acl[1];  // Mac OS X knows only one kind of ACL: extended
   } ExtMetaData;

#elif defined (__FreeBSD__)

   #define fnocache(fd) fcntl(fd, F_SETFL, O_DIRECT)

   typedef struct
   {
      exa_t  exa[2]; // FreeBSD got 2 name spaces for extended attributes: system and user
      acl_t  acl[3]; // FreeBSD knows 3 types of ACLs: access, default, and nfs4
   } ExtMetaData;

#endif

void getMetaData(const char *src, ExtMetaData *xmd);
void setMetaData(const char *dst, ExtMetaData *xmd);
void freeMetaData(ExtMetaData *xmd);


#pragma mark ••• AVL Tree •••

typedef struct Node
{
   // payload
   ulong  key;
   char  *name;
   int    dev;

   // house holding
   int          B;
   struct Node *L, *R;
} Node;


Node *findTreeNode(ulong key, Node  *node);
int    addTreeNode(ulong key, const char *name, int dev, Node **node, Node **passed);
int removeTreeNode(ulong key, Node **node);
void      freeTree(Node *node);


#pragma mark ••• Hash Table •••

Node    **createTable(uint n);
Node  *findTableEntry(Node *table[], ulong key);
Node   *addTableEntry(Node *table[], ulong key, const char *name, int dev);
void removeTableEntry(Node *table[], ulong key);
void        freeTable(Node *table[]);
