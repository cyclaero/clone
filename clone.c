//  clone.c
//  clone
//
//  Created by Dr. Rolf Jansen on 2013-01-13.
//  Copyright (c) 2013-2015. All rights reserved.
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

/*
   ToDo:
   - for backups of remote read-only stores, fetch a copy of the remote file tree.
   - add a command line option for forcing uid:gid+flags.
*/


#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <pwd.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/acl.h>

#include "utils.h"


static const char *version = "Version 1.0.7b (r"STRINGIFY(SVNREV)")";

// Device and file system informations
int   *gSourceFSType;
bool   gSourceRdOnly;
dev_t  gSourceDev;

int   *gDestinFSType;
dev_t  gDestinDev;

int    gVerbosityLevel = 1;
int    gErrorCount     = 0;

// Mode Flags
bool   gReadNoCache    = false;
bool   gWriteNoCache   = false;
bool   gIncremental    = false;
bool   gSynchronize    = false;

llong  gWriterLast     = 0;
llong  gTotalItems     = 0;
double gTotalSize      = 0.0;

Node **gHLinkINodes    = NULL;
Node **gExcludeList    = NULL;

// Thread synchronization
bool   gRunning        = true;
bool   gQItemWaitFlag  = false;
bool   gChunkWaitFlag  = false;
bool   gReaderWaitFlag = false;
bool   gWriterWaitFlag = false;
bool   gLevelWaitFlag  = false;

pthread_t       reader_thread;
pthread_t       writer_thread;
pthread_attr_t  thread_attrib;

pthread_mutex_t qitem_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  qitem_cond   = PTHREAD_COND_INITIALIZER;

pthread_mutex_t chunk_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  chunk_cond   = PTHREAD_COND_INITIALIZER;

pthread_mutex_t reader_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  reader_cond  = PTHREAD_COND_INITIALIZER;

pthread_mutex_t writer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  writer_cond  = PTHREAD_COND_INITIALIZER;

pthread_mutex_t level_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  level_cond   = PTHREAD_COND_INITIALIZER;


static inline void pthread_cond_wait_flag(pthread_cond_t *cond, pthread_mutex_t *mutex, bool *flag)
{
   if (*flag)
      pthread_cond_signal(cond);
   else
      *flag = true;
   pthread_cond_wait(cond, mutex);
   *flag = false;
}

// The chunk dispenser works very much like a towel dispenser featuring
// an integrated recycling unit. It dispenses empty chunks, informs the
// chunk currently being worked on, and it recycles finished chunks.
// Actually, the recycling procedure is as simple as setting the state
// flag to empty. This won't work with towels, since towel users won't
// accept dirty towels only being marked clean :-)

#define chunkBlckSize 1048576
#define maxChunkCount 100

enum {empty, ready, final};

#pragma pack(16)

typedef struct CopyChunk
{
   int    state;
   int    rc;
   size_t size;
   char   buffer[chunkBlckSize];
} CopyChunk;

CopyChunk gChunkDispenser[maxChunkCount];

#pragma pack()

int gBaseChunkSN = 0;
int gHighChunkSN = maxChunkCount;
int gNextChunkSN = 0;

CopyChunk *dispenseEmptyChunk(void)
{
   CopyChunk *chunk;

   pthread_mutex_lock(&chunk_mutex);
   while (gNextChunkSN == gHighChunkSN)
      pthread_cond_wait_flag(&chunk_cond, &chunk_mutex, &gChunkWaitFlag);

   chunk = &gChunkDispenser[gNextChunkSN];
   chunk->state = empty;

   if (++gNextChunkSN >= maxChunkCount)
   {
      gNextChunkSN = 0;
      gHighChunkSN = gBaseChunkSN;
   }
   pthread_mutex_unlock(&chunk_mutex);

   return chunk;
}

CopyChunk *informCurrentChunk(void)
{
   CopyChunk *chunk;

   pthread_mutex_lock(&chunk_mutex);
   chunk = &gChunkDispenser[gBaseChunkSN];
   if (chunk->state == empty)
      chunk = NULL;
   pthread_mutex_unlock(&chunk_mutex);

   return chunk;
}

void recycleFinishedChunk(CopyChunk *chunk)
{
   if (chunk)
   {
      int chunkSN = (int)(((intptr_t)chunk - (intptr_t)gChunkDispenser)/sizeof(CopyChunk));

      pthread_mutex_lock(&chunk_mutex);
      if (chunkSN >= gBaseChunkSN)
      {
         chunk->state = empty;

         gBaseChunkSN = chunkSN + 1;
         if (gBaseChunkSN >= maxChunkCount)
         {
            gBaseChunkSN %= maxChunkCount;
            gHighChunkSN  = maxChunkCount;
         }
         else if (gHighChunkSN != maxChunkCount)
            gHighChunkSN = gBaseChunkSN;

         if (gChunkWaitFlag && gNextChunkSN < gHighChunkSN)
            pthread_cond_signal(&chunk_cond);
      }
      pthread_mutex_unlock(&chunk_mutex);
   }
}


#define maxQItemCount 100

#pragma pack(16)

enum {virgin, nascent, reading, writing, skipping};

typedef struct
{
   int         stage;
   llong       fid;
   int         sfd,  dfd;
   char       *src, *dst;
   struct stat st;
   CopyChunk  *chunk;
} QItem;

QItem gQItemDispenser[maxQItemCount];

#pragma pack()

int gBaseQItemSN = 0;
int gHighQItemSN = maxQItemCount;
int gNextQItemSN = 0;

QItem *dispenseNewQItem(void)
{
   QItem *qitem;

   pthread_mutex_lock(&qitem_mutex);
   while (gNextQItemSN == gHighQItemSN)
      pthread_cond_wait_flag(&qitem_cond, &qitem_mutex, &gQItemWaitFlag);

   qitem = &gQItemDispenser[gNextQItemSN];

   if (++gNextQItemSN >= maxQItemCount)
   {
      gNextQItemSN = 0;
      gHighQItemSN = gBaseQItemSN;
   }
   pthread_mutex_unlock(&qitem_mutex);

   return qitem;
}

QItem *informReadingQItem(void)
{
   QItem *qitem;

   pthread_mutex_lock(&qitem_mutex);
   qitem = &gQItemDispenser[gBaseQItemSN];
   if (qitem->stage != nascent)
      qitem = NULL;
   pthread_mutex_unlock(&qitem_mutex);

   return qitem;
}

QItem *informWritingQItem(void)
{
   QItem *qitem;

   pthread_mutex_lock(&qitem_mutex);
   qitem = &gQItemDispenser[gBaseQItemSN];
   if (qitem->stage != writing && qitem->stage != skipping)
      qitem = NULL;
   pthread_mutex_unlock(&qitem_mutex);

   return qitem;
}

void recycleFinishedQItem(QItem *qitem)
{
   if (qitem)
   {
      int qitemSN = (int)(((intptr_t)qitem - (intptr_t)gQItemDispenser)/sizeof(QItem));

      pthread_mutex_lock(&qitem_mutex);
      if (qitemSN >= gBaseQItemSN)
      {
         qitem->stage = virgin;

         gBaseQItemSN = qitemSN + 1;
         if (gBaseQItemSN >= maxQItemCount)
         {
            gBaseQItemSN %= maxQItemCount;
            gHighQItemSN  = maxQItemCount;
         }
         else if (gHighQItemSN != maxQItemCount)
            gHighQItemSN = gBaseQItemSN;

         if (gQItemWaitFlag && gNextQItemSN < gHighQItemSN)
            pthread_cond_signal(&qitem_cond);
      }
      pthread_mutex_unlock(&qitem_mutex);
   }
}


static inline void feedback(const char action, const char *path)
{
   switch (gVerbosityLevel)
   {
      case 0:
         return;

      default:
      case 1:
      case 2:
         putc(action, stdout);
         fflush(stdout);
         break;

      case 3:
         printf("%c %s\n", action, path);
         fflush(stdout);
         break;
   }
}


#pragma mark ••• Passing the Attributes •••

void setAttributes(int sfd, int dfd, const char *src, const char *dst, struct stat *st)
{
   ExtMetaData xmd;
   getMetaData(sfd, src, st, &xmd);
   if (sfd != -1)
      close(sfd);

   setMetaData(dfd, dst, &xmd);
   if (dfd != -1)
      close(dfd);

   lchown(dst, st->st_uid, st->st_gid);
   lchmod(dst, modperms(st->st_mode));
   setTimesFlags(dst, st);

   if (gVerbosityLevel >= 2 && !S_ISDIR(st->st_mode))
      feedback('.', dst);
}


#pragma mark ••• Threaded Copying •••

int atomCopy(char *src, char *dst, struct stat *st);

void *reader(void *threadArg)
{
   static char errorString[errStrLen];

   while (gRunning)
   {
      QItem *qitem;

      pthread_mutex_lock(&reader_mutex);
      while (!(qitem = informReadingQItem()))         // wait for new items that are ready for reading
         pthread_cond_wait_flag(&reader_cond, &reader_mutex, &gReaderWaitFlag);
      qitem->stage = reading;                         // ready for reading now
      pthread_mutex_unlock(&reader_mutex);


      int in = open(qitem->src, O_RDONLY);
      if (in != -1)
      {
         qitem->sfd = in;

         if (gReadNoCache)
            fnocache(in);

         int        state;
         size_t     size;
         CopyChunk *chunk;

         do
         {
            chunk = dispenseEmptyChunk();
            if ((size = read(in, chunk->buffer, chunkBlckSize)) != -1)
            {
               chunk->rc    = NO_ERROR;
               chunk->size  = size;
               chunk->state = state = (size < chunkBlckSize) ? final : ready;
            }

            else
            {
               // Nothing good can be done here. Only drop a, message, and set the chunk size to 0.
               // The writer has to resolve the issue. Perhaps it is already waiting for the next chunk,
               // and therefore, the process may not be stopped here.
               chunk->rc    = SRC_ERROR;
               chunk->size  = 0;
               chunk->state = state = final;
               gErrorCount++;
               strerror_r(abs(chunk->rc), errorString, errStrLen);
               printf("\nRead error on file %s: %s.\n", qitem->src, errorString);
            }

            pthread_mutex_lock(&writer_mutex);
            if (qitem->stage == reading)
            {
               qitem->chunk = chunk;
               qitem->stage = writing;       // ready for writing now
            }
            if (gWriterWaitFlag)
               pthread_cond_signal(&writer_cond);
            pthread_mutex_unlock(&writer_mutex);
         } while (state != final);
      }

      else // (in == -1)
      {
         // Most probably, the file has been deleted after it was scheduled for copying.
         // Drop a message, and tell the writer to skip this one.
         gErrorCount++;
         strerror_r(errno, errorString, errStrLen);
         printf("\nFile %s could not be opened for reading: %s.\n", qitem->src, errorString);

         pthread_mutex_lock(&writer_mutex);
         close(qitem->sfd); qitem->sfd = -1;
         qitem->stage = skipping;            // do not write anything, only clean-up
         if (gWriterWaitFlag)
            pthread_cond_signal(&writer_cond);
         pthread_mutex_unlock(&writer_mutex);
      }
   }

   return NULL;
}


void *writer(void *threadArg)
{
   static char errorString[errStrLen];

   while (gRunning)
   {
      QItem *qitem;
      llong  fid;

      pthread_mutex_lock(&writer_mutex);
      while (!(qitem = informWritingQItem()))         // wait for items that are ready for writing
         pthread_cond_wait_flag(&writer_cond, &writer_mutex, &gWriterWaitFlag);
      fid = qitem->fid;
      pthread_mutex_unlock(&writer_mutex);

      if (qitem->stage == skipping)                   // check for an invalidated qitem
         goto cleanup;                                // skip writing

      CopyChunk *chunk = qitem->chunk;

      int out = open(qitem->dst, O_WRONLY|O_CREAT|O_TRUNC|O_EXLOCK, modperms(qitem->st.st_mode));
      if (out != -1)
      {
         qitem->dfd = out;

         if (gWriteNoCache)
            fnocache(out);

         int    state, rc = NO_ERROR;
         size_t fsize = 0;
         do
         {
            state = chunk->state;

            if (chunk->rc != NO_ERROR)
               rc = chunk->rc;

            else if (chunk->size == 0 || write(out, chunk->buffer, chunk->size) != -1)
            {
               fsize += chunk->size;
               recycleFinishedChunk(chunk);

               if (state != final)
               {
                  pthread_mutex_lock(&writer_mutex);
                  while (!(chunk = informCurrentChunk()))
                     pthread_cond_wait_flag(&writer_cond, &writer_mutex, &gWriterWaitFlag);
                  pthread_mutex_unlock(&writer_mutex);
               }
            }

            else
            {
               gErrorCount++;
               strerror_r(abs(rc = DST_ERROR), errorString, errStrLen);
               printf("\nWrite error on file %s: %s.\n", qitem->dst, errorString);
            }
         } while (state != final && rc == NO_ERROR);

         if (rc == NO_ERROR)
         {
            gTotalItems++;
            gTotalSize += fsize;

            if (fsize != qitem->st.st_size &&               // if the file size changed then most probably
                lstat(qitem->src, &qitem->st) != NO_ERROR)  // other things changed too, so lstat() again.
               rc = SRC_ERROR;
         }

         else // (rc != NO_ERROR)
         {
            // 1. clean up
            close(qitem->dfd); qitem->dfd = -1;
            close(qitem->sfd); qitem->sfd = -1;

            while (state != final)
            {
               recycleFinishedChunk(chunk);

               pthread_mutex_lock(&writer_mutex);
               while (!(chunk = informCurrentChunk()))
                  pthread_cond_wait_flag(&writer_cond, &writer_mutex, &gWriterWaitFlag);
               state = chunk->state;
               pthread_mutex_unlock(&writer_mutex);
            }

            // 2. try again using the atomic file copy routine
            rc = atomCopy(qitem->src, qitem->dst, &qitem->st);
         }

         // Error check again -- the atomic file copy may have resolved a previous issue
         if (rc == NO_ERROR)
            setAttributes(qitem->sfd, qitem->dfd, qitem->src, qitem->dst, &qitem->st);

         else // (rc != NO_ERROR)
         {
            gErrorCount++;
            strerror_r(abs(rc), errorString, errStrLen);
            printf("\nFile %s could not be copied to %s: %s.\n", qitem->src, qitem->dst, errorString);
         }
      }

      else // (out == -1)
      {
         close(qitem->sfd); qitem->sfd = -1;

         // Drop a message, remove the file from the queue, and clean-up without further notice.
         gErrorCount++;
         strerror_r(errno, errorString, errStrLen);
         printf("\nFile %s could not be opened for writing: %s.\n", qitem->dst, errorString);

         int state;
         do
         {
            state = chunk->state;
            recycleFinishedChunk(chunk);

            if (state != final)
            {
               pthread_mutex_lock(&writer_mutex);
               while (!(chunk = informCurrentChunk()))
                  pthread_cond_wait_flag(&writer_cond, &writer_mutex, &gWriterWaitFlag);
               pthread_mutex_unlock(&writer_mutex);
            }
         } while (state != final);
      }

   cleanup:
      deallocate_batch(false, VPR(qitem->dst), VPR(qitem->src), NULL);
      recycleFinishedQItem(qitem);

      pthread_mutex_lock(&reader_mutex);
      if (gReaderWaitFlag)
         pthread_cond_signal(&reader_cond);
      pthread_mutex_unlock(&reader_mutex);

      pthread_mutex_lock(&level_mutex);
      gWriterLast = fid;
      if (gLevelWaitFlag)
         pthread_cond_signal(&level_cond);
      pthread_mutex_unlock(&level_mutex);
   }

   return NULL;
}


void fileCopyScheduler(llong fid, char *src, char *dst, struct stat *st)
{
   QItem *qitem = dispenseNewQItem();

   pthread_mutex_lock(&reader_mutex);
   qitem->stage = nascent;
   qitem->fid   = fid;
   qitem->sfd   = -1;
   qitem->dfd   = -1;
   qitem->src   = src;
   qitem->dst   = dst;
   qitem->st    = *st;
   if (gReaderWaitFlag)
      pthread_cond_signal(&reader_cond);
   pthread_mutex_unlock(&reader_mutex);
}


#pragma mark ••• Atomic Copying •••

int atomCopy(char *src, char *dst, struct stat *st)
{
   int in, out;
   if ((in = open(src, O_RDONLY)) != -1)
      if ((out = open(dst, O_WRONLY|O_CREAT|O_TRUNC|O_EXLOCK, modperms(st->st_mode))) != -1)
      {
         if (gReadNoCache)
            fnocache(in);
         if (gWriteNoCache)
            fnocache(out);

         int    rc = NO_ERROR;
         size_t size, filesize = 0;
         char  *buffer = allocate(chunkBlckSize, false);

         do
         {
            if ((size = read(in, buffer, chunkBlckSize)) == -1)
               rc = SRC_ERROR;
            else if (size != 0 && write(out, buffer, size) == -1)
               rc = DST_ERROR;
            else
               filesize += size;
         } while (rc == NO_ERROR && size == chunkBlckSize);

         deallocate(VPR(buffer), false);

         close(out);
         close(in);

         gTotalItems++;
         gTotalSize += filesize;

         if (rc == NO_ERROR && filesize != st->st_size &&   // if the filesize changed then most probably
             lstat(src, st) != NO_ERROR)                    // other things changed too, so lstat() again.
            rc = SRC_ERROR;

         return rc;
      }

      else  // !out
      {
         close(in);
         return DST_ERROR;
      }

   else     // !in
      return SRC_ERROR;
}


int hlnkCopy(char *src, char *dst, size_t dl, struct stat *st)
{
   int   rc;
   Node *ino = findINode(gHLinkINodes, st->st_ino);

   if (ino && ino->value.pl.i == st->st_dev)
   {
      chflags(ino->name, 0);
      rc = (link(ino->name, dst) == NO_ERROR) ? 0 : DST_ERROR;
      gTotalItems++;
   }

   else
   {
      storeINode(gHLinkINodes, st->st_ino, dst, dl, st->st_dev);
      rc = atomCopy(src, dst, st);
   }

   if (rc == NO_ERROR)
      setAttributes(-1, -1, src, dst, st);

   deallocate_batch(false, VPR(dst), VPR(src), NULL);
   return rc;
}


int fileEmpty(char *src, char *dst, struct stat *st)
{
   int rc;
   int out = open(dst, O_WRONLY|O_CREAT|O_TRUNC|O_EXLOCK, modperms(st->st_mode));
   if (out != -1)
   {
      setAttributes(-1, out, src, dst, st);
      gTotalItems++;
      rc = NO_ERROR;
   }
   else
      rc = DST_ERROR;

   deallocate_batch(false, VPR(dst), VPR(src), NULL);
   return rc;
}


int slnkCopy(char *src, char *dst, struct stat *st)
{
   int    rc = NO_ERROR;
   char  *revpath = NULL;
   size_t revlen, maxlen = 1024;

   // 1. reveal the path pointed to by the link
   revpath = allocate(maxlen, false);
   while ((revlen = readlink(src, revpath, maxlen)) >= maxlen)
   {
      deallocate(VPR(revpath), false);
      if ((maxlen += maxlen) <= 1048576)
         revpath = allocate(maxlen += maxlen, false);
      else
      {
         revpath = NULL;      // if the length of the path to be revealed does'nt fit yet
         rc = -1;             // into 1 MB, then there is something screwed in our system
         goto cleanup;
      }
   }

   if (revlen == -1)
   {
      rc = SRC_ERROR;
      goto cleanup;
   }
   else
      revpath[revlen] = '\0';

   // 2. create a new symlink at dst pointing to the revealed path
   if (symlink(revpath, dst) == NO_ERROR)
   {
      setAttributes(-1, -1, src, dst, st);
      gTotalItems++;
   }
   else
      rc = DST_ERROR;

cleanup:
   deallocate_batch(false, VPR(revpath), VPR(dst), VPR(src), NULL);
   return rc;
}


#pragma mark ••• Directory Traversal •••

int dtType2stFmt(int d_type)
{
   switch (d_type)
   {
      default:
      case DT_UNKNOWN:  //  0 - The type is unknown.
         return 0;
      case DT_FIFO:     //  1 - A named pipe or FIFO.
         return DT_FIFO;
      case DT_CHR:      //  2 - A character device.
         return DT_CHR;
      case DT_DIR:      //  4 - A directory.
         return S_IFDIR;
      case DT_BLK:      //  6 - A block device.
         return S_IFBLK;
      case DT_REG:      //  8 - A regular file.
         return S_IFREG;
      case DT_LNK:      // 10 - A symbolic link.
         return S_IFLNK;
      case DT_SOCK:     // 12 - A local-domain socket.
         return S_IFSOCK;
      case DT_WHT:      // 14 - A whiteout file. (somehow deleted, but not eventually yet)
         return S_IFWHT;
   }
}

int deleteDirectory(char *path, size_t pl);

int deleteDirEntity(char *path, size_t pl, llong st_mode)
{
   static char errorString[errStrLen];
   const char *ftype;
   int err, rc = NO_ERROR;

   switch (st_mode & S_IFMT)
   {
      case S_IFDIR:      // A directory.
         *(short *)&path[pl++] = *(short *)"/";
         chflags(path, 0);
         if (err = deleteDirectory(path, pl))
            rc = err;
         else if (rmdir(path) != NO_ERROR)
         {
            rc = errno;
            gErrorCount++;
            strerror_r(rc, errorString, errStrLen);
            printf("\nDirectory %s could not be deleted: %s.\n", path, errorString);
         }
         else
            feedback('-', path);
         break;

      case S_IFIFO:      // A named pipe or FIFO.
      case S_IFREG:      // A regular file.
      case S_IFLNK:      // A symbolic link.
      case S_IFSOCK:     // A local-domain socket.
      case S_IFWHT:      // A whiteout file. (somehow deleted, but not eventually yet)
         lchflags(path, 0);
         if (unlink(path) != NO_ERROR)
         {
            rc = errno;
            gErrorCount++;
            strerror_r(rc, errorString, errStrLen);
            printf("\nFile %s could not be deleted: %s.\n", path, errorString);
         }
         break;

      case S_IFCHR:      // A character device.
         ftype = "a character device";
         goto special;
      case S_IFBLK:      // A block device.
         ftype = "a block device";
         goto special;
      default:
         ftype = "of unknown type";
      special:
         printf("\n%s is %s, it could not be deleted.\n", path, ftype);
         break;
   }

   return rc;
}

int deleteDirectory(char *path, size_t pl)
{
   int rc = NO_ERROR;

   DIR   *dp;
   struct dirent bep, *ep;

   if (dp = opendir(path))
   {
      struct stat st;
      while (readdir_r(dp, &bep, &ep) == 0 && ep)
         if ( ep->d_name[0] != '.' || (ep->d_name[1] != '\0' &&
             (ep->d_name[1] != '.' ||  ep->d_name[2] != '\0')))
         {
            // next path
            size_t npl   = pl + ep->d_namlen;
            char  *npath = strcpy(allocate(npl+2, false), path); strcpy(npath+pl, ep->d_name);

            if (ep->d_type != DT_UNKNOWN)
               rc = deleteDirEntity(npath, npl, dtType2stFmt(ep->d_type));
            else if (lstat(npath, &st) != -1)
               rc = deleteDirEntity(npath, npl, st.st_mode);
            else
               rc = DST_ERROR;

            deallocate(VPR(npath), false);
         }

      closedir(dp);
   }

   return rc;
}

int deleteEntityTree(Node *syncNode, const char *path, size_t pl)
{
   int rcL, rcR, rc = rcL = rcR = NO_ERROR;

   if (syncNode->L)
      rcL = deleteEntityTree(syncNode->L, path, pl);
   if (syncNode->R)
      rcR = deleteEntityTree(syncNode->R, path, pl);

   size_t npl   = pl + strlen(syncNode->name);
   char  *npath = strcpy(allocate(npl+2, false), path); strcpy(npath+pl, syncNode->name);
   rc = deleteDirEntity(npath, npl, syncNode->value.pl.i);

   deallocate_batch(false, VPR(npath), VPR(syncNode->name), VPR(syncNode), NULL);
   return (rcL != NO_ERROR)
          ? rcL
          : (rcR != NO_ERROR)
            ? rcR
            : rc;
}


void clone(const char *src, size_t sl, const char *dst, size_t dl, struct stat *st)
{
   static char    errorString[errStrLen];
   static llong   fileID = 0;
   struct stat    sstat, dstat;

   DIR           *sdp, *ddp;
   struct dirent *sep, *dep, bep;

   // In incremental or sysnchronize mode, store the inventory of
   // the destination directory into the key-name/value store.
   Node *syncNode, **syncEntities = NULL;
   if ((gIncremental || gSynchronize) && (ddp = opendir(dst)))
   {
      Value  value = {{.i = 0}, Simple, NULL};
      syncEntities = createTable(1024);
      while (readdir_r(ddp, &bep, &dep) == NO_ERROR && dep)
         if ( dep->d_name[0] != '.' || (dep->d_name[1] != '\0' &&
             (dep->d_name[1] != '.' ||  dep->d_name[2] != '\0')))
         {
            if (dep->d_type == DT_DIR || dep->d_type == DT_REG || dep->d_type == DT_LNK)
            {
               value.pl.i = dtType2stFmt(dep->d_type);
               storeFSName(syncEntities, dep->d_name, dep->d_namlen, &value);
            }

            else if (dep->d_type == DT_UNKNOWN)    // need to call lstat()
            {
               char path[dl+dep->d_namlen+1]; strcpy(path, dst); strcpy(path+dl, dep->d_name);
               if (lstat(path, &dstat) != -1 &&
                   ((dstat.st_mode &= S_IFMT) == S_IFDIR || dstat.st_mode == S_IFREG || dstat.st_mode == S_IFLNK))
               {
                  value.pl.i = dstat.st_mode;
                  storeFSName(syncEntities, dep->d_name, dep->d_namlen, &value);
               }
            }
         }

      closedir(ddp);
   }

   if (sdp = opendir(src))
   {
      int   rc;
      llong lastID = 0;
      llong fCount = 0;
      const char *ftype;

      while (readdir_r(sdp, &bep, &sep) == NO_ERROR && sep)
         if ( sep->d_name[0] != '.' || (sep->d_name[1] != '\0' &&
             (sep->d_name[1] != '.' ||  sep->d_name[2] != '\0')))
         {
            if (gExcludeList && findFSName(gExcludeList, sep->d_name, sep->d_namlen))
            {
               if (syncEntities)
                  removeFSName(syncEntities, sep->d_name, sep->d_namlen);
               continue;
            }

            // next source path
            size_t nsl  = sl + sep->d_namlen;   // next source length
            char  *nsrc = strcpy(allocate(nsl+2, false), src); strcpy(nsrc+sl, sep->d_name);
            if (gExcludeList && findFSName(gExcludeList, nsrc, nsl) || lstat(nsrc, &sstat) != NO_ERROR)
            {
               if (syncEntities)
                  removeFSName(syncEntities, sep->d_name, sep->d_namlen);
               deallocate(VPR(nsrc), false);
               continue;
            }

            // next destination path
            size_t ndl  = dl + sep->d_namlen;   // next destination length
            char  *ndst = strcpy(allocate(ndl+2, false), dst); strcpy(ndst+dl, sep->d_name);

            dstat.st_ino = 0;
            if (syncEntities &&                 // only non-NULL in incremental or synchronize mode
                (syncNode = findFSName(syncEntities, sep->d_name, sep->d_namlen)))
            {
               removeFSName(syncEntities, sep->d_name, sep->d_namlen);

               // This file system item is already present at the destination,
               // but is it really exactly the same file or entity?
               if (lstat(ndst, &dstat) == NO_ERROR       &&
                   dstat.st_uid        == sstat.st_uid   &&
                   dstat.st_gid        == sstat.st_gid   &&
                   dstat.st_mode       == sstat.st_mode  &&
                   dstat.st_flags      == sstat.st_flags &&
                   (S_ISDIR(dstat.st_mode)                                   ||
                    dstat.st_size              == sstat.st_size              &&
                    dstat.st_mtimespec.tv_sec  == sstat.st_mtimespec.tv_sec  &&
                    dstat.st_mtimespec.tv_nsec == sstat.st_mtimespec.tv_nsec &&
                    (dstat.st_birthtimespec.tv_sec  == -1                            ||
                     sstat.st_birthtimespec.tv_sec  == -1                            ||
                     dstat.st_birthtimespec.tv_sec  == sstat.st_birthtimespec.tv_sec &&
                     dstat.st_birthtimespec.tv_nsec == sstat.st_birthtimespec.tv_nsec
                    )
                   )
                  )
               {
                  // Yes, it is, so if it is not a directory then skip it!
                  if (!S_ISDIR(dstat.st_mode))
                  {
                     if (S_ISREG(sstat.st_mode) && sstat.st_nlink > 1)
                     {
                        Node *ino = findINode(gHLinkINodes, sstat.st_ino);
                        if (!ino || ino->value.pl.i != sstat.st_dev)
                           storeINode(gHLinkINodes, sstat.st_ino, ndst, ndl, sstat.st_dev);
                     }

                     deallocate_batch(false, VPR(ndst), VPR(nsrc), NULL);
                     continue;
                  }
               }

               else if (deleteDirEntity(ndst, ndl, dstat.st_mode) == NO_ERROR)
                  // No, it has the same name, but it is somehow different.
                  // So, try to delete it from the destination before continuing.
                  dstat.st_ino = 0;

               else
               {
                  // If it cannot be deleted, it cannot be overwritten either.
                  // A message has already been dropped, and there is nothing
                  // good left to be done here, so skip this one.
                  deallocate_batch(false, VPR(ndst), VPR(nsrc), NULL);
                  continue;
               }
            }

            bool dirCreated = false;

            switch (sstat.st_mode & S_IFMT)
            {
               case S_IFDIR:      // A directory.
                  if (((dstat.st_ino != 0 || lstat(ndst, &dstat) == NO_ERROR) && S_ISDIR(dstat.st_mode)
                     || dstat.st_ino == 0 && (dirCreated = (mkdir(ndst, sstat.st_mode&ALLPERMS | S_IWUSR) == NO_ERROR)) && lstat(ndst, &dstat) == NO_ERROR)
                     && dstat.st_dev == gDestinDev)
                  {
                     if (dirCreated)
                     {
                        gTotalItems++;
                        feedback('+', ndst);
                     }
                     else
                        feedback('=', ndst);

                     *(short *)&nsrc[nsl++] = *(short *)&ndst[ndl++] = *(short *)"/";
                     if (sstat.st_dev == gSourceDev)
                        clone(nsrc, nsl, ndst, ndl, &sstat);
                     else
                        setAttributes(-1, -1, nsrc, ndst, &sstat);
                  }

                  else
                  {
                     rc = errno;
                     gErrorCount++;
                     strerror_r(abs(rc), errorString, errStrLen);
                     printf("\nDestination directory %s could not be created: %s.\n", ndst, errorString);
                  }

                  deallocate_batch(false, VPR(ndst), VPR(nsrc), NULL);
                  break;

               case S_IFREG:      // A regular file.
                  if (sstat.st_nlink == 1)
                     if (sstat.st_size > 0)
                     {
                        fCount++;
                        lastID = ++fileID;
                        fileCopyScheduler(fileID, nsrc, ndst, &sstat);
                     }

                     else // (sstat.st_size == 0)
                     {
                        if ((rc = fileEmpty(nsrc, ndst, &sstat)) != NO_ERROR)
                        {
                           gErrorCount++;
                           strerror_r(abs(rc), errorString, errStrLen);
                           printf("\nCreating file %s in %s failed: %s.\n", sep->d_name, dst, errorString);
                        }
                     }

                  else // (sstat.st_nlink > 1)
                     if ((rc = hlnkCopy(nsrc, ndst, ndl, &sstat)) != NO_ERROR)
                     {
                        gErrorCount++;
                        strerror_r(abs(rc), errorString, errStrLen);
                        printf("\nCopying file or hard link %s from %s to %s failed: %s.\n", sep->d_name, src, dst, errorString);
                     }
                  break;

               case S_IFLNK:      // A symbolic link.
                  if ((rc = slnkCopy(nsrc, ndst, &sstat)) != NO_ERROR)
                  {
                     gErrorCount++;
                     strerror_r(abs(rc), errorString, errStrLen);
                     printf("\nCopying symbolic link %s from %s to %s failed: %s.\n", sep->d_name, src, dst, errorString);
                  }
                  break;

               case S_IFIFO:      // A named pipe or FIFO.
                  ftype = "a named pipe or FIFO";
                  goto special;
               case S_IFCHR:      // A character device.
                  ftype = "a character device";
                  goto special;
               case S_IFBLK:      // A block device.
                  ftype = "a block device";
                  goto special;
               case S_IFSOCK:     // A local-domain socket.
                  ftype = "a local-domain socket";
                  goto special;
               case S_IFWHT:      // A whiteout file. (somehow deleted, but not eventually yet)
                  ftype = "a unsupported whiteout file";
                  goto special;
               default:
                  ftype = "of unknown type";
               special:
                  if (gVerbosityLevel > 0)
                     printf("\n%s is %s, and it is not copied.\n", nsrc, ftype);
                  deallocate_batch(false, VPR(ndst), VPR(nsrc), NULL);
                  break;
            }
         }

      closedir(sdp);

      if (syncEntities)
      {
         if (gIncremental)
            releaseTable(syncEntities);

         else if (gSynchronize)
         {
            // need to delete entities not been treated above
            uint i, n = *(uint *)syncEntities;
            for (i = 1; i <= n; i++)
               if (syncEntities[i])
                  deleteEntityTree(syncEntities[i], dst, dl);
            deallocate(VPR(syncEntities), false);
         }
      }

      if (fCount)
      {
         pthread_mutex_lock(&level_mutex);
         while (lastID > gWriterLast)
            pthread_cond_wait_flag(&level_cond, &level_mutex, &gLevelWaitFlag);
         pthread_mutex_unlock(&level_mutex);
      }

      setAttributes(-1, -1, src, dst, st);
   }
}


#pragma mark ••• Main •••

bool checkIntersection(char *src, char *dst)
{
   char srp[PATH_MAX], drp[PATH_MAX];

   if (realpath(src, srp))
      if (realpath(dst, drp))
      {
         size_t srl = strlen(srp);
         size_t drl = strlen(drp);
         if (strstr(drp, srp) == drp && (srl == drl || drp[srl] == '/' || srl == 1 && *drp == '/'))
            printf("Absolute destination '%s' must not be a child of the source path '%s'.\n", drp, srp);
         else
            return false;
      }

      else
         printf("Destination path %s is invalid.\n", dst);

   else
      printf("Source path %s is invalid.\n", src);

   return true;  // path intersection or path error
}


bool ynPrompt(const char *request, const char *object, int defponse)
{
   printf(request, object, defponse);

   int response = getchar();
   if (response == '\n' || response == '\r')
      response = defponse;

   return (response == 'y' || response == 'Y') ? true : false;
}


void usage(const char *executable)
{
   const char *r = executable + strlen(executable);
   while (--r >= executable && *r != '/')
      ;
   r++;
   printf("File tree cloning by Dr. Rolf Jansen (c) 2013-2015 - %s\n\n", version);
   printf("\
Usage: %s [-c roff|woff|rwoff] [-d|-i|-s] [-v level] [-x exclude-list] [-X excl-list-file] [-y] [-h|-?|?] source/ destination/\n\n\
       -c roff|woff|rwoff  Selectively turn off the file system cache for reading or writing\n\
                           or for reading and writing -- the caches are on by default.\n\n\
       The options -d, -i, -s are mutually exclusive:\n\
       -d                  Delete the contents of the destination before cloning, but do not\n\
                           remove the destination directory or mount point itself. Stop on error.\n\n\
       -i                  Incrementally add new content to or change content in the destination,\n\
                           but do not touch content in destination that does not exist in source.\n\n\
       -s                  Completely synchronize destination with source.\n\n\
       -v level            Verbosity level (default = 1):\n\
                           0 - no output\n\
                           1 - show directory action: + for add, - for delete, = for keep\n\
                           2 - indicate cloned files by '.'\n\
                           3 - display the path names of cloned file system items\n\n\
       -x exclude-list     Colon separated list of entity names or full path names to be\n\
                           excluded from cloning. Use full path names to single out exactly\n\
                           one item. Use entity names, if all existing entities having that name\n\
                           should be excluded.\n\
                           For example:  -x \".snap:/.sujournal:.DS_Store:/fullpath/to a/volatile cache\"\n\n\
       -X excl-list-file   File containig a list of entity names or full path names to be excluded -- one item per line.\n\n\
       -y                  Automatically answer with y(es) to y|n confirmation prompts.\n\n\
       -h|-?|?             Show these usage instructions.\n\n\
       source/             Path to the source directory or moint point. The final '/' may be omitted.\n\n\
       destination/        Path to the destination directory or moint point. If the destination\n\
                           does not exist, then it will be created. The final '/' may be omitted.\n\n", r);
}


extern ssize_t gAllocationTotal;

int main(int argc, char *const argv[])
{
   int    optchr, vLevel;
   size_t homelen = 0;
   char   ch, *o, *p, *q, *usrhome = "", *cmd = argv[0];
   bool   d_flag = false, i_flag = false, s_flag = false, y_flag = false;
   FILE  *exclf;
   struct stat exclst;

   while ((optchr = getopt(argc, argv, "c:disv:x:X:yh?")) != -1)
      switch (optchr)
      {
         case 'c':   // selectively turn off the file system cache for reading or writing or for reading and writing
            if (strcmp(optarg, "roff") == 0)
               gReadNoCache = true;
            else if (strcmp(optarg, "woff") == 0)
               gWriteNoCache = true;
            else if (strcmp(optarg, "rwoff") == 0)
               gReadNoCache = gWriteNoCache = true;
            else
               goto arg_err;
            break;

         case 'd':   // delete the contents of the destination before cloning
            if (i_flag || s_flag)
               goto arg_err;
            else
               d_flag = true;
            break;

         case 'i':   // incrementally add new content to or change content in the destination
            if (d_flag || s_flag)
               goto arg_err;
            else
               i_flag = gIncremental = true;
            break;

         case 's':   // completely synchronize source and destination
            if (d_flag || i_flag)
               goto arg_err;
            else
               s_flag = gSynchronize = true;
            break;

         case 'v':   // verbosity level
            vLevel = atoi(optarg);
            if (0 <= vLevel && vLevel <= 3)
               gVerbosityLevel = vLevel;
            else
               goto arg_err;
            break;

         case 'x':   // colon separated list of entity names or full path names to be excluded from cloning
            if (!gExcludeList)
               gExcludeList = createTable(256);
            q = p = optarg;
            do
            {
               while ((ch = *q) && ch != ':')
                  q++;
               *q++ = '\0';
               if (*p)
                  storeFSName(gExcludeList, p, strlen(p), NULL);
               p = q;
            } while (ch);
            break;

         case 'X':
            if (*(short *)optarg == *(short *)"~/")
            {
               usrhome = getpwuid(getuid())->pw_dir;
               homelen = strlen(usrhome);
               strcpy(o = alloca(homelen+strlen(optarg)+1), usrhome);
               strcpy(o+homelen, optarg+1);
            }
            else
               o = optarg;

            if (stat(o, &exclst) == NO_ERROR &&
                (exclf = fopen(o, "r")))
            {
               if (exclst.st_size)
               {
                  if (!gExcludeList)
                     gExcludeList = createTable(256);

                  p = alloca(exclst.st_size + 1);
                  if (fread(p, (size_t)exclst.st_size, 1, exclf) == 1)
                  {
                     for (q = p + exclst.st_size - 1; q > p && (*q == '\n' || *q == '\r'); q--);   // strip trailing line breaks
                     if (q > p)
                     {
                        *(q+1) = '\0';
                        q = p;
                        do
                        {
                           while ((ch = *q) && ch != '\n' && ch != '\r')
                              q++;
                           *q++ = '\0';
                           if (*p)
                              storeFSName(gExcludeList, p, strlen(p), NULL);
                           p = q;
                        } while (ch);
                     }
                  }
               }

               fclose(exclf);
            }

            else
               goto arg_err;
            break;

         case 'y':
            y_flag = true;
            break;

         case 'h':
         case '?':
         default:
         arg_err:
            usage(cmd);
            return 1;
      }

   argc -= optind;
   argv += optind;

   if (argc < 2 || argc && argv[0][0] == '?' && argv[0][1] == '\0')
   {
      usage(cmd);
      return 1;
   }

   // 1. check whether to deal with paths in the home directory
   bool stilde = *(short *)argv[0] == *(short *)"~/";
   bool dtilde = *(short *)argv[1] == *(short *)"~/";
   if (!homelen && (stilde || dtilde))
   {
      usrhome = getpwuid(getuid())->pw_dir;
      homelen = strlen(usrhome);
   }

   size_t as = strlen(argv[0]), sl = (stilde) ? homelen + as - 1 : as;
   size_t ad = strlen(argv[1]), dl = (dtilde) ? homelen + ad - 1 : ad;
   char   src[sl+2];
   char   dst[dl+2];
   if (stilde)
   {
      strcpy(src, usrhome);
      strcpy(src+homelen, argv[0]+1);
   }
   else
      strcpy(src, argv[0]);

   if (dtilde)
   {
      strcpy(dst, usrhome);
      strcpy(dst+homelen, argv[1]+1);
   }
   else
      strcpy(dst, argv[1]);

   if (src[sl-1] != '/')
      *(short *)&src[sl++] = *(short *)"/";
   if (dst[dl-1] != '/')
      *(short *)&dst[dl++] = *(short *)"/";

   if (gVerbosityLevel)
      printf("File tree cloning by Dr. Rolf Jansen (c) 2013-2015 - %s\nclone %s %s\n", version, src, dst);

   // 2. check whether the paths do exist, lead to directories, and make sure that destination is not inherited by source
   bool   dirCreated = false;
   int    rc;
   struct stat sstat, dstat;
   if (lstat(src, &sstat) != NO_ERROR)
      printf("The source directory %s does not exist.\n", argv[0]);

   else if (!S_ISDIR(sstat.st_mode))
      printf("Source %s is not a directory.\n", argv[0]);

   else if (lstat(dst, &dstat) != NO_ERROR &&
            ((d_flag = !(dirCreated = (mkdir(dst, sstat.st_mode&ALLPERMS | S_IWUSR) == NO_ERROR))) ||   // in the case of a successfull mkdir(), d_flag is set to false
             lstat(dst, &dstat) != NO_ERROR))                                                           // this prevents deleteDir() from running on an empty directory.
      printf("The destination directory %s did not exist and a new one could not be created: %s.\n", argv[1], strerror(errno));

   else if (!dirCreated && !S_ISDIR(dstat.st_mode))
      printf("Destination %s is not a directory.\n", argv[1]);

   else if (sstat.st_dev == dstat.st_dev && checkIntersection(src, dst))
   {
      // exact failure message has been output in checkIntersection()
      if (dirCreated)
         rmdir(dst);
   }

   else if (d_flag && !y_flag && !ynPrompt("Do you really want to delete the content of %s?\nThe deletion cannot be undone. -- y(es, do delete) | n(o, do not delete) [%c]?: ", argv[1], 'n'))
      printf("The deletion of the content of %s was not confirmed by the user.\n", argv[1]);

   else if (d_flag && (rc = deleteDirectory(dst, dl)) != NO_ERROR)
      printf("The content of the destination directory %s could not be deleted: %s.\n", argv[1], strerror(rc));

   else
   {
      int    i;
      struct timeval t0, t1;
      gettimeofday(&t0, NULL);

      struct statfs sfs;
      statfs(src, &sfs);
      gSourceFSType = (int *)sfs.f_fstypename;
      gSourceRdOnly = sfs.f_flags & MNT_RDONLY;
      gSourceDev    = sstat.st_dev;

      struct statfs dfs;
      statfs(dst, &dfs);
      gDestinFSType = (int *)dfs.f_fstypename;
      gDestinDev    = dstat.st_dev;

      gHLinkINodes = createTable(8192);

      // Initialze the dispensers
      for (i = 0; i < maxChunkCount; i++)
         gChunkDispenser[i].state = empty;

      for (i = 0; i < maxQItemCount; i++)
         gQItemDispenser[i].stage = virgin;

      pthread_attr_init(&thread_attrib);
      pthread_attr_setstacksize(&thread_attrib, 8*1048576);
      pthread_attr_setdetachstate(&thread_attrib, PTHREAD_CREATE_DETACHED);

      if (rc = pthread_create(&reader_thread, &thread_attrib, reader, NULL))
      {
         printf("Cannot create file reader thread: %s.\n", strerror(rc));
         return 1;
      }

      if (rc = pthread_create(&writer_thread, &thread_attrib, writer, NULL))
      {
         printf("Cannot create file writer thread: %s.\n",  strerror(rc));
         return 1;
      }

      if (dirCreated)
      {
         gTotalItems++;
         feedback('+', dst);
      }
      else
         feedback('=', dst);

      clone(src, sl, dst, dl, &sstat);

      gRunning = false;
      pthread_cond_signal(&reader_cond);
      pthread_cond_signal(&writer_cond);
      releaseTable(gHLinkINodes);
      releaseTable(gExcludeList);

      if (gVerbosityLevel)
      {
         if (gTotalItems)
         {
            gettimeofday(&t1, NULL);
            double t = t1.tv_sec - t0.tv_sec + (t1.tv_usec - t0.tv_usec)/1.0e6;
            gTotalSize /= 1048576.0;
            if (gTotalItems != 1)
               printf("\n%llu items copied, %.1f MB in %.2f s -- %.1f MB/s\n", gTotalItems, gTotalSize, t, gTotalSize/t);
            else
               printf("\n1 item copied, %.1f MB in %.2f s -- %.1f MB/s\n", gTotalSize, t, gTotalSize/t);
         }
         else
            printf("\nNo items copied.\n");

         printf("Leaked memory: %zd bytes\n", gAllocationTotal);

         switch (gErrorCount)
         {
            case 0:
               printf("No errors occured.\n");
               break;

            case 1:
               printf("One error occured.\n");
               break;

            default:
               printf("%d errors occured.\n", gErrorCount);
               break;
         }
      }

      return gErrorCount;
   }

   return -1;
}
