//  utils.h
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


#define errStrLen   256

#define  NO_ERROR   0
#define SRC_ERROR   -errno       // negative errors indicate a problem with the source.
#define DST_ERROR   +errno       // positive errors indicate a problem with the destination.


#define ENQUOTING(cvar) #cvar
#define STRINGIFY(cvar) ENQUOTING(cvar)

#define modperms(mode) (mode&ALLPERMS | ((gSourceRdOnly) ? S_IWUSR : 0))


typedef unsigned char      uchar;
typedef unsigned short     ushort;
typedef unsigned int       uint;
typedef unsigned long      ulong;

typedef long long          llong;
typedef unsigned long long ullong;


void setTimesFlags(const char *dst, struct stat *st);


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

#pragma mark ••• Value Data Types •••

// Data Types in the key/name-value store -- negative values are dynamic.
enum 
{
   dynamic    = -1,  // multiply kind with -1 if the data has been dynamically allocated
   Empty      =  0,  // the key is the data
   Simple     =  1,  // boolean, integer, floating point, etc. values
   Data       =  2,  // any kind of structured or unstructured data
   String     =  3,  // a nul terminated string
   Dictionary =  5,  // a dictionary table, i.e. another key/name-value store
};

typedef struct
{
   long    kind;  // negative kinds indicate dynamically allocated data
   union
   {
      bool    b;  // a boolean value
      long    i;  // an integer
      double  d;  // a floating point number
      time_t  t;  // a time stamp
      char   *s;  // a string
      void   *p;  // a pointer to anything
   } v;
} Value;

void releaseValue(Value *value);


typedef struct Node
{
   // keys
   ulong  key;    // if this is zero, then
   char  *name;   // use name as the key.

   // value
   Value  value;

   // house holding
   int          B;
   struct Node *L, *R;
} Node;


// CAUTION: It is an error to call these functions with key being 0 AND name being NULL.
//          Either of both must be non-zero. No error cheking is done within these recursive functions.
Node *findTreeNode(ulong key, const char *name, Node  *node);
int    addTreeNode(ulong key, const char *name, ssize_t namlen, Value *value, Node **node, Node **passed);
int removeTreeNode(ulong key, const char *name, Node **node);
void   releaseTree(Node *node);


#pragma mark ••• Hash Tables •••

Node **createTable(uint n);
void  releaseTable(Node *table[]);

Node    *findINode(Node *table[], ulong key);
Node   *storeINode(Node *table[], ulong key, const char *fsname, ssize_t namlen, long dev);
void   removeINode(Node *table[], ulong key);

Node   *findFSName(Node *table[], const char *fsname, ssize_t namlen);
Node  *storeFSName(Node *table[], const char *fsname, ssize_t namlen, Value *value);
void  removeFSName(Node *table[], const char *fsname, ssize_t namlen);
