#define NPROC        64  // maximum number of processes
#define NTHREAD      64  // maxinum number of threads
#define KSTACKSIZE 4096  // size of per-process kernel stack
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#define FSSIZE       40000  // size of file system in blocks
#define MIN_CPU_SHARE 20
#define NTICKET      100
#define NSHARE       10000
#define STRIDE_TICK   5
#define QUEUE_TICK(x) (5*(1<<x))
#define LOWER_TICK(x) (x*20+20)
#define BOOST_TICK    200
#define REINITIATE_TICK 2000000000
