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

// negative errors indicate a problem with the source.
#define src_error -errno

// positive errors indicate a problem with the destination.
#define dst_error +errno


dev_t  gSrcDev;
llong  gWriterLast = 0;

bool   gRunning    = true;

bool   gQItemWait  = false;
bool   gChunkWait  = false;
bool   gReaderWait = false;
bool   gWriterWait = false;
bool   gLevelWait  = false;

double gTotalSize  = 0.0;

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
         // fnocache(in);

         size_t filesize = qitem->st.st_size;
         size_t blcksize = chunkBlckSize;
         size_t headsize = (filesize%blcksize) ?: blcksize;
         uint   i, n = (uint)((filesize-headsize)/blcksize);

         CopyChunk *chunk = dispenseEmptyChunk();
         if (read(in, chunk->buffer, headsize) == headsize)
         {
            chunk->state = (n == 0) ? final : ready;
            chunk->rc    = no_error;
            chunk->size  = headsize;
         }

         else
         {
            // Nothing good can be done here. Only drop a,
            // message, and set the chunk size to 0, so
            // the writer can resolve the issue.
            chunk->state = final;
            chunk->rc    = src_error;
            chunk->size  = 0;
            printf("\nRead error on file %s: %d.\n", qitem->src, chunk->rc);
         }

         pthread_mutex_lock(&writer_mutex);
         qitem->chunk = chunk;
         if (gWriterWait)
            pthread_cond_signal(&writer_cond);
         pthread_mutex_unlock(&writer_mutex);

         // the continuation chunks if any
         for (i = 1; i <= n && chunk->rc == no_error; i++)
         {
            chunk = dispenseEmptyChunk();
            if (read(in, chunk->buffer, blcksize) == blcksize)
            {
               chunk->state = (n == i) ? final : ready;
               chunk->rc    = no_error;
               chunk->size  = blcksize;
            }

            else
            {
               // Again, the writer needs to resolve the issue.
               // Perhaps it is already waiting for the next chunk,
               // and therefore the processe may not be stopped here.
               chunk->state = final;
               chunk->rc    = src_error;
               chunk->size  = 0;
               printf("\nRead error on file %s: %d.\n", qitem->src, chunk->rc);
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
         // Drop a message, release any allocated memory, mark the queue item invalid,
         // so the writer does ignore it.
         printf("\nFile #%lld, %s could not be opened for reading: %d.\n", qitem->fid, qitem->src, src_error);

         gWriterLast = qitem->fid;
         qitem->fid = -1;
         free(qitem->src);
         free(qitem->dst);
      }
   }

   return NULL;
}


void *writer(void *threadArg)
{
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

      if (qitem.fid == -1)                            // skipping invalidated queue items
         continue;

      CopyChunk *chunk = qitem.chunk;

      int out = open(qitem.dst, O_WRONLY|O_CREAT|O_TRUNC|O_EXLOCK, qitem.st.st_mode & ALLPERMS);
      if (out != -1)
      {
         // fnocache(out);

         int state, rc = no_error;

         do
         {
            state = chunk->state;

            if (chunk->rc != no_error)
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
               rc = dst_error;
               printf("\nWrite error on file %s: %d.\n", qitem.dst, rc);
            }

         } while (state != final && rc == no_error);

         close(out);

         if (rc == no_error)
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
         if (rc == no_error)
            setAttributes(qitem.src, qitem.dst, &qitem.st);

         else
            printf("\nFile %s could not be copied to %s: %d.\n", qitem.src, qitem.dst, rc);

         free(qitem.src);
         free(qitem.dst);
      }

      else
      {
         // Drop a message, remove the file from the queue, and clean-up without further notice.
         printf("\nFile %s could not be opened for writing: %d.\n", qitem.dst, dst_error);

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
   int    in, out, rc = no_error;
   size_t filesize = st->st_size;

   if ((in = open(src, O_RDONLY)) != -1)
      if ((out = open(dst, O_WRONLY|O_CREAT|O_TRUNC|O_EXLOCK, st->st_mode & ALLPERMS)) != -1)
      {
         // fnocache(in);
         // fnocache(out);

         if (filesize)
         {
            size_t blcksize = chunkBlckSize;
            size_t tailsize = filesize%blcksize;
            uint i, n = (uint)(filesize/blcksize);
            char *buffer = malloc(blcksize);

            for (i = 0; i < n && rc == no_error; i++)
               if (read(in, buffer, blcksize) != blcksize)
                  rc = src_error;
               else if (write(out, buffer, blcksize) != blcksize)
                  rc = dst_error;

            if (tailsize && rc == no_error)
               if (read(in, buffer, tailsize) != tailsize)
                  rc = src_error;
               else if (write(out, buffer, tailsize) != tailsize)
                  rc = dst_error;

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
        return dst_error;
      }

   else     // !in
      return src_error;
}


Node **gLinkInodes = NULL;

int hlnkCopy(char *src, char *dst, struct stat *st)
{
   int    rc;
   Node  *ino = findTableEntry(gLinkInodes, st->st_ino);

   if (ino && st->st_dev == ino->dev)
   {
      chflags(ino->name, 0);
      rc = (link(ino->name, dst) == no_error) ? 0 : dst_error;
   }

   else
   {
      addTableEntry(gLinkInodes, st->st_ino, dst, st->st_dev);
      rc = atomCopy(src, dst, st);
   }

   if (rc == no_error)
      setAttributes(src, dst, st);

   free(src);
   free(dst);

   return rc;
}


void fileEmpty(char *src, char *dst, struct stat *st)
{
   close(open(dst, O_WRONLY|O_CREAT|O_TRUNC|O_EXLOCK, st->st_mode & ALLPERMS));
   setAttributes(src, dst, st);
   free(src);
   free(dst);
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
   int    rc = no_error;
   char  *revpath = NULL;
   size_t revlen, maxlen = 1024;
   struct stat st;

   if (lstat(src, &st) == no_error)
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
            rc = -sys_error;     // into 1 MB, then there is something screwed in our system
            goto cleanup;
         }
      }

      if (revlen == -1)
      {
         rc = src_error;
         goto cleanup;
      }

      else
         revpath[revlen] = '\0';

      // 2. create a new symlink at dst pointing to the revealed path
      if (symlink(revpath, dst) == no_error)
         setAttributes(src, dst, &st);
      else
         rc = dst_error;
   }

cleanup:
   free(revpath);
   free(src);
   free(dst);
   return rc;
}


void clone(const char *src, size_t sl, const char *dst, size_t dl)
{
   static llong  fileID = 0;
          llong  lastID = 0;
          llong  fCount = 0;

   DIR          *dp;
   struct dirent bep, *ep;
   struct stat   st;

   if (dp = opendir(src))
   {
      while (readdir_r(dp, &bep, &ep) == no_error && ep)
      {
         // next source and destination paths
         size_t  nsl = sl + ep->d_namlen;  // next source length
         size_t  ndl = dl + ep->d_namlen;  // next destination length

         char *nsrc = strcpy(malloc(nsl+2), src); strcpy(nsrc+sl, ep->d_name);
         char *ndst = strcpy(malloc(ndl+2), dst); strcpy(ndst+dl, ep->d_name);

         const char *ftype;

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
                   && stat(nsrc, &st) == no_error
                   && mkdir(ndst, st.st_mode & ALLPERMS) == no_error)
               {
                  putc('.', stdout); fflush(stdout);
                  *(short *)&nsrc[nsl++] = *(short *)&ndst[ndl++] = *(short *)"/";
                  if (st.st_dev == gSrcDev)
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
               if (stat(nsrc, &st) == no_error &&
                   strcmp(ep->d_name, ".sujournal") != 0)
                  if (st.st_nlink == 1)
                  {
                     if (st.st_size > 0)
                     {
                        fCount++;
                        lastID = ++fileID;
                        fileCopy(fileID, nsrc, ndst, &st);
                     }
                     else
                        fileEmpty(nsrc, ndst, &st);
                  }

                  else
                  {
                     if (hlnkCopy(nsrc, ndst, &st) != no_error)
                        printf("\nCopying file or hard link %s from %s to %s failed.\n", ep->d_name, src, dst);
                  }

               else
               {
                  free(nsrc);
                  free(ndst);
               }
               break;

            case DT_LNK:      // 10 - A symbolic link.
               if (slnkCopy(nsrc, ndst) != no_error)
                  printf("\nCopying symbolic link %s from %s to %s failed.\n", ep->d_name, src, dst);
               break;
         }
      }

      closedir(dp);

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


int main(int argc, const char *argv[])
{
   if (argc != 3)
      printf("File tree cloning by Dr. Rolf Jansen\nCyclaero Ltda. (c) 2013 - %s\nUsage:  clone source destination\n\n", svnrev);

   else
   {
      // 1. check whether to deal with paths in the home directory
      size_t homelen = 0;
      char  *usrhome;
      bool   stilde = *(short *)argv[1] == *(short *)"~/";
      bool   dtilde = *(short *)argv[2] == *(short *)"~/";
      if (stilde || dtilde)
      {
         usrhome = getpwuid(getuid())->pw_dir;
         homelen = strlen(usrhome);
      }

      size_t as = strlen(argv[1]), sl = (stilde) ? homelen + as - 1 : as;
      size_t ad = strlen(argv[2]), dl = (dtilde) ? homelen + ad - 1 : ad;
      char   src[sl+2];
      char   dst[dl+2];
      if (stilde)
      {
         strcpy(src, usrhome);
         strcpy(src+homelen, argv[1]+1);
      }
      else
         strcpy(src, argv[1]);

      if (dtilde)
      {
         strcpy(dst, usrhome);
         strcpy(dst+homelen, argv[2]+1);
      }
      else
         strcpy(dst, argv[2]);

      // 2. check whether the paths do exist and lead to directories
      struct stat sstat, dstat;
      if (stat(src, &sstat) != no_error)
         printf("The source directory %s does not exist.\n", argv[1]);

      else if ((sstat.st_mode & S_IFMT) != S_IFDIR)
         printf("Source %s is not a directory.\n", argv[1]);

      else if (stat(dst, &dstat) != no_error &&
               (mkdir(dst, sstat.st_mode & ALLPERMS) != no_error || stat(dst, &dstat) != no_error))
         printf("The destination directory %s did not exist and a new one could not be created.\n", argv[2]);

      else if ((dstat.st_mode & S_IFMT) != S_IFDIR)
         printf("Destination %s is not a directory.\n", argv[2]);

      else
      {
         int    i, rc;
         struct timeval t0, t1;
         gettimeofday(&t0, NULL);

         gSrcDev = sstat.st_dev;
         gLinkInodes = createTable(8192);

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
            printf("Cannot create file reader thread: %d.", rc);
            return 1;
         }

         if (rc = pthread_create(&writer_thread, &thread_attrib, writer, NULL))
         {
            printf("Cannot create file writer thread: %d.", rc);
            return 1;
         }

         if (src[sl-1] != '/')
            *(short *)&src[sl++] = *(short *)"/";
         if (dst[dl-1] != '/')
            *(short *)&dst[dl++] = *(short *)"/";
         printf("File tree cloning by Dr. Rolf Jansen\nCyclaero Ltda. (c) 2013 - %s\n\nclone %s %s\n", svnrev, src, dst);

         putc('.', stdout); fflush(stdout);
         clone(src, sl, dst, dl);
         setAttributes(src, dst, &sstat);

         gRunning = false;
         pthread_cond_signal(&reader_cond);
         pthread_cond_signal(&writer_cond);
         freeTable(gLinkInodes);

         gettimeofday(&t1, NULL);
         double t = t1.tv_sec - t0.tv_sec + (t1.tv_usec - t0.tv_usec)/1.0e6;
         gTotalSize /= 1048576.0;
         printf("\n%.1f MB in %.2f s -- %.1f MB/s\n\n", gTotalSize, t, gTotalSize/t);

         return 0;
      }
   }

   return 1;
}
