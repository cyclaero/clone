//  clone.c
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
#include <sys/acl.h>

#include "utils.h"


static const char *svnrev = "(r"STRINGIFY(SVNREV)")";

dev_t  gSourceDev;

// Mode Flags
bool   gReadNoCache  = false;
bool   gWriteNoCache = false;
bool   gIncremental  = false;
bool   gSynchronize  = false;

llong  gWriterLast   = 0;
double gTotalSize    = 0.0;

Node **gLinkINodes   = NULL;
Node **gExcludeList  = NULL;

// Thread synchronization
bool   gRunning      = true;
bool   gQItemWait    = false;
bool   gChunkWait    = false;
bool   gReaderWait   = false;
bool   gWriterWait   = false;
bool   gLevelWait    = false;

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


// Our chunk dispenser is very much like a towel dispenser that features
// an integrated recycling unit. It dispenses empty chunks, informs the
// chunk currently being worked on, and it recycles finished chunks.
// Actually, our recycling process is as simple as setting the state flag
// to empty. This won't work with towels, since towel users won't accept
// dirty towels only being marked clean :-)
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
   {
      gChunkWait = true;
      pthread_cond_wait(&chunk_cond, &chunk_mutex);
      gChunkWait = false;
   }

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
         
         if (gChunkWait && gNextChunkSN < gHighChunkSN)
            pthread_cond_signal(&chunk_cond);
      }
      pthread_mutex_unlock(&chunk_mutex);
   }
}


#define maxQItemCount 100

#pragma pack(16)

enum {virgin = -1, nascent, reading, writing};

typedef struct
{
   int          stage;
   llong        fid;
   char        *src, *dst;
   struct stat  st;
   CopyChunk   *chunk;
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
   {
      gQItemWait = true;
      pthread_cond_wait(&qitem_cond, &qitem_mutex);
      gQItemWait = false;
   }

   qitem = &gQItemDispenser[gNextQItemSN];
   qitem->stage = nascent;
   qitem->chunk = NULL;

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
   if (!qitem->chunk || (qitem->stage != reading && qitem->stage != writing))
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
         qitem->chunk = NULL;

         gBaseQItemSN = qitemSN + 1;
         if (gBaseQItemSN >= maxQItemCount)
         {
            gBaseQItemSN %= maxQItemCount;
            gHighQItemSN  = maxQItemCount;
         }

         else if (gHighQItemSN != maxQItemCount)
            gHighQItemSN = gBaseQItemSN;

         if (gQItemWait && gNextQItemSN < gHighQItemSN)
            pthread_cond_signal(&qitem_cond);
      }
      pthread_mutex_unlock(&qitem_mutex);
   }
}


int atomCopy(char *src, char *dst, struct stat *st);

void *reader(void *threadArg)
{
   static char errorString[errStrLen];

   while (gRunning)
   {
      QItem *qitem;

      pthread_mutex_lock(&reader_mutex);
      while ((qitem = informReadingQItem()) == NULL)  // wait for new items that are ready for reading
      {
         gReaderWait = true;
         pthread_cond_wait(&reader_cond, &reader_mutex);
         gReaderWait = false;
      }
      qitem->stage = reading;
      pthread_mutex_unlock(&reader_mutex);


      int in = open(qitem->src, O_RDONLY);
      if (in != -1)
      {
         if (gReadNoCache) fnocache(in);

         size_t filesize = qitem->st.st_size;
         size_t blcksize = chunkBlckSize;
         size_t headsize = (filesize%blcksize) ?: blcksize;
         uint   i, n = (uint)((filesize-headsize)/blcksize);

         CopyChunk *chunk = dispenseEmptyChunk();
         if (read(in, chunk->buffer, headsize) == headsize)
         {
            chunk->state = (n == 0) ? final : ready;
            chunk->rc    = NO_ERROR;
            chunk->size  = headsize;
         }

         else
         {
            // Nothing good can be done here. Only drop a,
            // message, and set the chunk size to 0, so
            // the writer can resolve the issue.
            chunk->state = final;
            chunk->rc    = SRC_ERROR;
            chunk->size  = 0;
            strerror_r(abs(chunk->rc), errorString, errStrLen);
            printf("\nRead error on file %s: %s.\n", qitem->src, errorString);
         }

         pthread_mutex_lock(&writer_mutex);
         qitem->chunk = chunk;
         if (gWriterWait)
            pthread_cond_signal(&writer_cond);
         pthread_mutex_unlock(&writer_mutex);

         // the continuation chunks if any
         for (i = 1; i <= n && chunk->rc == NO_ERROR; i++)
         {
            chunk = dispenseEmptyChunk();
            if (read(in, chunk->buffer, blcksize) == blcksize)
            {
               chunk->state = (n == i) ? final : ready;
               chunk->rc    = NO_ERROR;
               chunk->size  = blcksize;
            }

            else
            {
               // Again, the writer needs to resolve the issue.
               // Perhaps it is already waiting for the next chunk,
               // and therefore the processe may not be stopped here.
               chunk->state = final;
               chunk->rc    = SRC_ERROR;
               chunk->size  = 0;
               strerror_r(abs(chunk->rc), errorString, errStrLen);
               printf("\nRead error on file %s: %s.\n", qitem->src, errorString);
            }

            pthread_mutex_lock(&writer_mutex);
            if (gWriterWait)
               pthread_cond_signal(&writer_cond);
            pthread_mutex_unlock(&writer_mutex);
         }

         close(in);
      }

      else
      {
         // Most probably, the file has been deleted after it was scheduled for copying.
         // Drop a message, release any allocated memory, invalidate the queue items chunk,
         // so the writer does skip this one.
         strerror_r(errno, errorString, errStrLen);
         printf("\nFile %s could not be opened for reading: %s.\n", qitem->src, errorString);

         pthread_mutex_lock(&writer_mutex);
         qitem->chunk = INVALIDATED;
         if (gWriterWait)
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
      QItem *pqitem, qitem;

      pthread_mutex_lock(&writer_mutex);
      while ((pqitem = informWritingQItem()) == NULL) // wait for items that are ready for writing
      {
         gWriterWait = true;
         pthread_cond_wait(&writer_cond, &writer_mutex);
         gWriterWait = false;
      }

      pqitem->stage = writing;
      qitem = *pqitem;
      pthread_mutex_unlock(&writer_mutex);

      recycleFinishedQItem(pqitem);

      pthread_mutex_lock(&reader_mutex);
      if (gReaderWait)
         pthread_cond_signal(&reader_cond);
      pthread_mutex_unlock(&reader_mutex);

      if (qitem.chunk == INVALIDATED) // check for an invalidated chunk pointer
         goto finish;                 // skip writing

      CopyChunk *chunk = qitem.chunk;

      int out = open(qitem.dst, O_WRONLY|O_CREAT|O_TRUNC|O_EXLOCK, qitem.st.st_mode & ALLPERMS);
      if (out != -1)
      {
         if (gWriteNoCache) fnocache(out);

         int state, rc = NO_ERROR;

         do
         {
            state = chunk->state;

            if (chunk->rc != NO_ERROR)
               rc = chunk->rc;

            else if (write(out, chunk->buffer, chunk->size) == chunk->size)
            {
               recycleFinishedChunk(chunk);

               if (state != final)
               {
                  pthread_mutex_lock(&writer_mutex);
                  while (!(chunk = informCurrentChunk()))
                  {
                     gWriterWait = true;
                     pthread_cond_wait(&writer_cond, &writer_mutex);
                     gWriterWait = false;
                  }
                  pthread_mutex_unlock(&writer_mutex);
               }
            }

            else
            {
               strerror_r(abs(rc = DST_ERROR), errorString, errStrLen);
               printf("\nWrite error on file %s: %s.\n", qitem.dst, errorString);
            }

         } while (state != final && rc == NO_ERROR);

         close(out);

         if (rc == NO_ERROR)
            gTotalSize += qitem.st.st_size;

         else
         {
            // 1. clean up
            while (state != final)
            {
               recycleFinishedChunk(chunk);

               pthread_mutex_lock(&writer_mutex);
               while (!(chunk = informCurrentChunk()))
               {
                  gWriterWait = true;
                  pthread_cond_wait(&writer_cond, &writer_mutex);
                  gWriterWait = false;
               }
               state = chunk->state;
               pthread_mutex_unlock(&writer_mutex);
            }

            // 2. try again using the atomic file copy routine
            rc = atomCopy(qitem.src, qitem.dst, &qitem.st);
         }

         // Error check again -- the atomic file copy may have resolved a previous issue
         if (rc == NO_ERROR)
            setAttributes(qitem.src, qitem.dst, &qitem.st);

         else
         {
            strerror_r(abs(rc), errorString, errStrLen);
            printf("\nFile %s could not be copied to %s: %s.\n", qitem.src, qitem.dst, errorString);
         }

         free(qitem.src);
         free(qitem.dst);
      }

      else
      {
         // Drop a message, remove the file from the queue, and clean-up without further notice.
         strerror_r(errno, errorString, errStrLen);
         printf("\nFile %s could not be opened for writing: %s.\n", qitem.dst, errorString);

         free(qitem.src);
         free(qitem.dst);

         int state;
         do
         {
            state = chunk->state;
            recycleFinishedChunk(chunk);

            if (state != final)
            {
               pthread_mutex_lock(&writer_mutex);
               while (!(chunk = informCurrentChunk()))
               {
                  gWriterWait = true;
                  pthread_cond_wait(&writer_cond, &writer_mutex);
                  gWriterWait = false;
               }
               pthread_mutex_unlock(&writer_mutex);
            }
         } while (state != final);
      }

   finish:
      pthread_mutex_lock(&level_mutex);
      gWriterLast = qitem.fid;
      if (gLevelWait)
         pthread_cond_signal(&level_cond);
      pthread_mutex_unlock(&level_mutex);
   }

   return NULL;
}


int atomCopy(char *src, char *dst, struct stat *st)
{
   int    in, out, rc = NO_ERROR;
   size_t filesize = st->st_size;

   if ((in = open(src, O_RDONLY)) != -1)
      if ((out = open(dst, O_WRONLY|O_CREAT|O_TRUNC|O_EXLOCK, st->st_mode & ALLPERMS)) != -1)
      {
         if (gReadNoCache)  fnocache(in);
         if (gWriteNoCache) fnocache(out);

         if (filesize)
         {
            size_t blcksize = chunkBlckSize;
            size_t tailsize = filesize%blcksize;
            uint i, n = (uint)(filesize/blcksize);
            char *buffer = malloc(blcksize);

            for (i = 0; i < n && rc == NO_ERROR; i++)
               if (read(in, buffer, blcksize) != blcksize)
                  rc = SRC_ERROR;
               else if (write(out, buffer, blcksize) != blcksize)
                  rc = DST_ERROR;

            if (tailsize && rc == NO_ERROR)
               if (read(in, buffer, tailsize) != tailsize)
                  rc = SRC_ERROR;
               else if (write(out, buffer, tailsize) != tailsize)
                  rc = DST_ERROR;

            free(buffer);
         }

         close(out);
         close(in);
         gTotalSize += filesize;
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
   int    rc;
   Node  *ino = findINode(gLinkINodes, st->st_ino);

   if (ino && ino->value.i == st->st_dev)
   {
      chflags(ino->name, 0);
      rc = (link(ino->name, dst) == NO_ERROR) ? 0 : DST_ERROR;
   }

   else
   {
      storeINode(gLinkINodes, st->st_ino, dst, dl, st->st_dev);
      rc = atomCopy(src, dst, st);
   }

   if (rc == NO_ERROR)
      setAttributes(src, dst, st);

   free(src);
   free(dst);

   return rc;
}


int fileEmpty(char *src, char *dst, struct stat *st)
{
   int rc;
   int out = open(dst, O_WRONLY|O_CREAT|O_TRUNC|O_EXLOCK, st->st_mode & ALLPERMS);
   if (out != -1)
   {
      close(out);
      setAttributes(src, dst, st);
      rc = NO_ERROR;
   }
   else
      rc = DST_ERROR;

   free(src);
   free(dst);
   return rc;
}


void fileCopy(llong fid, char *src, char *dst, struct stat *st)
{
   QItem *qitem = dispenseNewQItem();
   pthread_mutex_lock(&reader_mutex);
   qitem->fid = fid;
   qitem->src = src;
   qitem->dst = dst;
   qitem->st  = *st;
   if (gReaderWait)
      pthread_cond_signal(&reader_cond);
   pthread_mutex_unlock(&reader_mutex);
}


int slnkCopy(char *src, char *dst)
{
   int    rc = NO_ERROR;
   char  *revpath = NULL;
   size_t revlen, maxlen = 1024;
   struct stat st;

   if (lstat(src, &st) == NO_ERROR)
   {
      // 1. reveal the path pointed to by the link
      revpath = malloc(maxlen);
      while ((revlen = readlink(src, revpath, maxlen)) >= maxlen)
      {
         free(revpath);
         if ((maxlen += maxlen) <= 1048576)
            revpath = malloc(maxlen += maxlen);
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
         setAttributes(src, dst, &st);
      else
         rc = DST_ERROR;
   }

cleanup:
   free(revpath);
   free(src);
   free(dst);
   return rc;
}


void clone(const char *src, size_t sl, const char *dst, size_t dl)
{
   static char   errorString[errStrLen];
   static llong  fileID = 0;

   DIR          *sdp;
   struct dirent bep, *ep;
   struct stat   st;

   if (sdp = opendir(src))
   {
      int   rc;
      llong lastID = 0;
      llong fCount = 0;
      const char *ftype;

      while (readdir_r(sdp, &bep, &ep) == NO_ERROR && ep)
      {
         if (gExcludeList && findFSName(gExcludeList, ep->d_name, ep->d_namlen))
            continue;

         // next source path
         size_t nsl  = sl + ep->d_namlen;  // next source length
         char  *nsrc = strcpy(malloc(nsl+2), src); strcpy(nsrc+sl, ep->d_name);

         if (gExcludeList && findFSName(gExcludeList, nsrc, nsl))
         {
            free(nsrc);
            continue;
         }

         // next destination path
         size_t ndl  = dl + ep->d_namlen;  // next destination length
         char  *ndst = strcpy(malloc(ndl+2), dst); strcpy(ndst+dl, ep->d_name);

         switch(ep->d_type)
         {
            case DT_UNKNOWN:  //  0 - The type is unknown.
            case DT_WHT:      // 14 - A whiteout file. (somehow deleted, but not eventually yet)
            default:
               printf("\n%s is of unsupported type %d.\n", nsrc, ep->d_type);
               free(nsrc);
               free(ndst);
               break;

            case DT_FIFO:     //  1 - A named pipe, or FIFO.
               ftype = "named pipe, or FIFO";
               goto special;

            case DT_CHR:      //  2 - A character device.
               ftype = "character device";
               goto special;

            case DT_BLK:      //  6 - A block device.
               ftype = "block device";
               goto special;

            case DT_SOCK:     // 12 - A local-domain socket.
               ftype = "local-domain socket";

            special:
               printf("\n%s is a %s, and it is not copied.\n", nsrc, ftype);
               free(nsrc);
               free(ndst);
               break;

            case DT_DIR:      //  4 - A directory.
               if ((ep->d_name[0] != '.' || (ep->d_name[1] != '\0' &&
                   (ep->d_name[1] != '.' ||  ep->d_name[2] != '\0')))
                   && stat(nsrc, &st) == NO_ERROR
                   && mkdir(ndst, st.st_mode & ALLPERMS) == NO_ERROR)
               {
                  putc('.', stdout); fflush(stdout);
                  *(short *)&nsrc[nsl++] = *(short *)&ndst[ndl++] = *(short *)"/";
                  if (st.st_dev == gSourceDev)
                  {
                     clone(nsrc, nsl, ndst, ndl);
                     setAttributes(nsrc, ndst, &st);
                  }
                  else
                  {
                     struct timeval tv[2];
                     chown(ndst, st.st_uid, st.st_gid);
                     chmod(ndst, st.st_mode & ALLPERMS);
                     utimes(ndst, utimeset(&st, tv));
                     chflags(ndst, st.st_flags);
                  }
               }

               free(nsrc);
               free(ndst);
               break;

            case DT_REG:      //  8 - A regular file.
               if (stat(nsrc, &st) == NO_ERROR)
                  if (st.st_nlink == 1)
                  {
                     if (st.st_size > 0)
                     {
                        fCount++;
                        lastID = ++fileID;
                        fileCopy(fileID, nsrc, ndst, &st);
                     }
                     else
                     {
                        rc = fileEmpty(nsrc, ndst, &st);
                        if (rc != NO_ERROR)
                        {
                           strerror_r(abs(rc), errorString, errStrLen);
                           printf("\nCreating file %s in %s failed: %s.\n", ep->d_name, dst, errorString);
                        }
                     }
                  }

                  else
                  {
                     rc = hlnkCopy(nsrc, ndst, ndl, &st);
                     if (rc != NO_ERROR)
                     {
                        strerror_r(abs(rc), errorString, errStrLen);
                        printf("\nCopying file or hard link %s from %s to %s failed: %s.\n", ep->d_name, src, dst, errorString);
                     }
                  }

               else
               {
                  free(nsrc);
                  free(ndst);
               }
               break;

            case DT_LNK:      // 10 - A symbolic link.
               rc = slnkCopy(nsrc, ndst);
               if (rc != NO_ERROR)
               {
                  strerror_r(abs(rc), errorString, errStrLen);
                  printf("\nCopying symbolic link %s from %s to %s failed: %s.\n", ep->d_name, src, dst, errorString);
               }
               break;
         }
      }

      closedir(sdp);

      if (fCount)
      {
         pthread_mutex_lock(&level_mutex);
         while (gWriterLast < lastID)
         {
            gLevelWait = true;
            pthread_cond_wait(&level_cond, &level_mutex);
            gLevelWait = false;
         }
         pthread_mutex_unlock(&level_mutex);
      }
   }
}


void usage(const char *executable)
{
   const char *r = executable + strlen(executable);
   while (r-- >= executable && *r != '/'); r++;
   printf("File tree cloning by Dr. Rolf Jansen, Cyclaero Ltda. (c) 2013 - %s\n\n", svnrev);
   printf("\
Usage: %s [-c roff|woff|rwoff] [-d|-i|-s] [-x exclude-list] [-X excl-list-file] [-h|-?|?] source/ destination/\n\n\
       -c roff|woff|rwoff  selectively turn off the file system cache for reading or writing\n\
                           or for reading and writing -- the caches are on by default.\n\n\
"/*
       The options -d, -i, -s are mutually exclusive:\n\
       -d                  erase the contents of the destination before cloning, but do not\n\
                           remove the destination directory or mount point itself. Stop on error.\n\n\
       -i                  incrementally add new content to or change content in the destination,\n\
                           but do not touch content in destination that does not exist in source.\n\n\
       -s                  completely synchronize source and destination.\n\n\
*/"\
       -x exclude-list     colon separated list of entity names or full path names to be\n\
                           excluded from cloning. Use full path names to single out exactly\n\
                           one item. Use entity names, if all existing entities having that name\n\
                           should be excluded.\n\
                           for example:  -x \".snap:/.sujournal:.DS_Store:/fullpath/to a/volatile cache\"\n\n\
       -X excl-list-file   file containig a list of entity names or full path names to be excluded, one item per line.\n\n\
       -h|-?|?             shows these usage instructions.\n\n\
       source/             path to the source directory or moint point. The '/' can be omitted.\n\
       destination/        path to the destination directory or moint point. The '/' can be omitted.\n\n", r);
}


int main(int argc, char *const argv[])
{
   int    optchr;
   size_t homelen = 0;
   char   ch, *o, *p, *q, *usrhome;
   bool   d_flag = false, i_flag = false, s_flag = false;
   FILE  *exclf;
   struct stat exclst;

   while ((optchr = getopt(argc, argv, "c:disx:X:h?")) != -1)
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
                  storeFSName(gExcludeList, p, strlen(p), 0);
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

                  o = p = alloca(exclst.st_size + 1);
                  if (fread(p, exclst.st_size, 1, exclf) == 1)
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
                              storeFSName(gExcludeList, p, strlen(p), 0);
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

         case 'h':
         case '?':
         default:
         arg_err:
            usage(argv[0]);
            return 1;
      }

   argc -= optind;
   argv += optind;

   if (argc && argv[0][0] == '?' && argv[0][1] == '\0' || argc < 2)
   {
      usage(argv[0]);
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

   // 2. check whether the paths do exist and lead to directories
   struct stat sstat, dstat;
   if (stat(src, &sstat) != NO_ERROR)
      printf("The source directory %s does not exist.\n", argv[0]);

   else if ((sstat.st_mode & S_IFMT) != S_IFDIR)
      printf("Source %s is not a directory.\n", argv[0]);

   else if (stat(dst, &dstat) != NO_ERROR &&
            (mkdir(dst, sstat.st_mode & ALLPERMS) != NO_ERROR || stat(dst, &dstat) != NO_ERROR))
      printf("The destination directory %s did not exist and a new one could not be created.\n", argv[1]);

   else if ((dstat.st_mode & S_IFMT) != S_IFDIR)
      printf("Destination %s is not a directory.\n", argv[1]);

   else
   {
      int    i, rc;
      struct timeval t0, t1;
      gettimeofday(&t0, NULL);

      gSourceDev  = sstat.st_dev;
      gLinkINodes = createTable(8192);

      // Initialze the dispensers
      for (i = 0; i < maxChunkCount; i++)
         gChunkDispenser[i].state = empty;

      for (i = 0; i < maxQItemCount; i++)
      {
         gQItemDispenser[i].stage = virgin;
         gQItemDispenser[i].chunk = NULL;
      }

      pthread_attr_init(&thread_attrib);
      pthread_attr_setstacksize(&thread_attrib, 1048576);
      pthread_attr_setdetachstate(&thread_attrib, PTHREAD_CREATE_DETACHED);

      if (rc = pthread_create(&reader_thread, &thread_attrib, reader, NULL))
      {
         printf("Cannot create file reader thread: %s.", strerror(rc));
         return 1;
      }

      if (rc = pthread_create(&writer_thread, &thread_attrib, writer, NULL))
      {
         printf("Cannot create file writer thread: %s.",  strerror(rc));
         return 1;
      }

      if (src[sl-1] != '/')
         *(short *)&src[sl++] = *(short *)"/";
      if (dst[dl-1] != '/')
         *(short *)&dst[dl++] = *(short *)"/";
      printf("File tree cloning by Dr. Rolf Jansen, Cyclaero Ltda. (c) 2013 - %s\nclone %s %s\n", svnrev, src, dst);

      putc('.', stdout); fflush(stdout);
      clone(src, sl, dst, dl);
      setAttributes(src, dst, &sstat);

      gRunning = false;
      pthread_cond_signal(&reader_cond);
      pthread_cond_signal(&writer_cond);
      releaseTable(gLinkINodes);
      releaseTable(gExcludeList);

      gettimeofday(&t1, NULL);
      double t = t1.tv_sec - t0.tv_sec + (t1.tv_usec - t0.tv_usec)/1.0e6;
      gTotalSize /= 1048576.0;
      printf("\n%.1f MB in %.2f s -- %.1f MB/s\n\n", gTotalSize, t, gTotalSize/t);

      return 0;
   }

   return 1;
}
