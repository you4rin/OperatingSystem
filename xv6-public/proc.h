// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
  //struct thread *thread;       // The thread running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  struct thread* recent;       // Recently used thread ptr
  int isDummy;                 // 1 if mlfq dummy process, 0 otherwise
  int level;                   // -1 if stride, 0 if high, 1 if mid, 2 if low
  int ticket;                  // Percentage of cpu share
  int stride;                  // NSHARE / stride
  int pass;                    // Total strides
  int tot_time;                // Total time in current queue
  int queue_time;              // Total time in queue front
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap

struct thread{
  uint tid;                    // Thread id
  struct proc *group;          // Group of thread
  void* retval;                // return value
  char *kstack;                // Bottom of kernel stack for this process
  char *ustack;                // Bottom of user stack for this process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  enum procstate state;        // Process state
};

struct Queue{
  uint sz;
  uint front;
  uint back;
  struct proc* arr[NPROC + 1];
};

struct MLFQ{
  uint tick;
  struct Queue q[3];
};

struct Stride{
  uint sz;
  struct proc* arr[NPROC + 1];
};
