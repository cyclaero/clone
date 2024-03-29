### [ACTION REQUIRED] Your GitHub account, cyclaero, will soon require 2FA

Here is the deal: https://obsigna.com/articles/1693258424.html

---
 
clone is a file tree cloning tool which runs 3 threads - a scheduler (main), a
reader, and a writer thread. Reading and writing occurs in parallel. While this
is most beneficial for copying data from one physical disk to another, clone is
also very well suited for cloning a file tree to any place on the same disk.

Cloning includes the whole directory hierarchy, i.e. sub-directories, files,
hard links, symbolic links, attributes (modes, flags, times), extended
attributes and access control lists.

clone is useful for cloning (thus backing-up) live file systems, and it can
also be used in incremental and synchronization mode.

clone works on FreeBSD and macOS.

clone is very fast, for example, cloning a whole UFS2 file hierarchy on
FreeBSD 9.1 of in total 2.3 TBytes of data from one hard disk to another
took 7.5 h, so the average transfer rate for all kind of files (very small
up to very big ones) was about 89 MByte/s.
