#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

/*
struct{
  struct spinlock lock;
  struct thread thread[NTHREAD];
} ttable;
*/

struct thread ttable[NTHREAD];

static struct proc *initproc;

int nexttid = 1;
int nextpid = 1;

struct MLFQ mlfq;
struct proc mlfq_manager;
struct Stride stride;

extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

struct thread*
ttable_begin()
{
  return ttable;
}

struct thread*
ttable_end()
{
  return ttable+NTHREAD;
}

int
thread_create(thread_t* thread, void* (*start_routine)(void*), void* arg)
{
  struct proc* p;
  struct thread* th;
  struct thread* nth;
  char* sp;
  uint sz;

  // allocproc part
  acquire(&ptable.lock);
  //cprintf("begin\n");
  p = myproc();
  th = mythread();

  for(nth = ttable_begin(); nth < ttable_end(); ++nth)
    if(nth->state == UNUSED && nth->group == p)
      goto found;
  
  for(nth = ttable_begin(); nth < ttable_end(); ++nth)
    if(nth->state == UNUSED && nth->group == 0)
      goto found;
  
  release(&ptable.lock);
  return -1;

found:
  //cprintf("found\n");
  //cprintf("selected idx: %d\n",nth-ttable);
  nth->state = EMBRYO;
  nth->tid = nexttid++;
  nth->group = p;
  nth->retval = 0;
  *thread = nth->tid;
  nth->chan = 0;

  if(nth->kstack != 0)
    goto kstack;
  if((nth->kstack = kalloc()) == 0){
    cprintf("cannot alloc kstack\n");
    goto bad;
  }

kstack:
  //cprintf("kstack\n");
  sp = nth->kstack + KSTACKSIZE;

  sp -= sizeof *(nth->tf);
  nth->tf = (struct trapframe*)sp;
  *(nth->tf) = *(th->tf);

  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *(nth->context);
  nth->context = (struct context*)sp;
  memset(nth->context, 0, sizeof *(nth->context));
  nth->context->eip = (uint)forkret;

  // fork part
  // exec part
  sz = PGROUNDUP(p->sz);

  if(nth->ustack != 0)
    goto ustack;
  if((sz = allocuvm(p->pgdir, sz, sz+PGSIZE)) == 0){
    cprintf("cannot alloc ustack\n");
    goto bad;
  }

ustack:
  //cprintf("ustack\n");
  nth->ustack = (char*)sz;
  sp = (char*)sz;

  sp -= 4;
  *(uint*)sp = (uint)arg;

  sp -= 4;
  *(uint*)sp = (uint)0xffffffff;

  p->sz = sz;
  nth->tf->eip = (uint)start_routine;
  nth->tf->esp = (uint)sp;
  nth->state = RUNNABLE;
  //cprintf("sp in tid %d: %p\n",nth->tid, sp);

  //cprintf("before end\n");
  release(&ptable.lock);
  //cprintf("end\n");
  return 0;

bad:
  if(nth->kstack != 0)
    kfree(nth->kstack);
  nth->kstack = 0;
  nth->state = UNUSED;
  nth->tid = 0;
  nth->group = 0;
  //cprintf("bad\n");
  release(&ptable.lock);
  //cprintf("???\n");
  return -1;
}

int 
thread_join(thread_t thread, void** retval)
{
  int havekids;
  struct proc* p = myproc();
  struct thread* th;

  //cprintf("join tid: %d\n", thread);
  acquire(&ptable.lock);
  for(;;){
    havekids = 0;
    for(th = ttable_begin(); th < ttable_end(); ++th){
      if(th->state == UNUSED || th->tid != thread || th->group != p)
        continue;
      havekids = 1;
      if(th->state == ZOMBIE){
        *retval = th->retval;
        if(th->kstack == 0){
          cprintf("kstack already deallocated\n");
          return -1;
        }
        kfree(th->kstack);
        th->kstack = 0;
        th->chan = 0;
        th->context = 0;
        //th->group = 0;
        th->tid = 0;
        th->retval = 0;
        th->state = UNUSED;
        th->tf = 0;
        release(&ptable.lock);
        return 0;
      }
      break;
    }
    if(!havekids || p->killed){
      release(&ptable.lock);
      return -1;
    }
    sleep((void*)th->tid, &ptable.lock);
  }
  return 0;
}

void 
thread_exit(void* retval)
{
  struct thread* curthread = mythread();

  //cprintf("exit tid: %d\n", curthread->tid);
  acquire(&ptable.lock);
  wakeup1((void*)curthread->tid);

  curthread->retval = retval;
  curthread->state = ZOMBIE;

  sched();
  panic("zombie exit");
}


int
tidx(int idx)
{
  return idx%NTHREAD;
}

int 
proceed(struct cpu* c,struct proc* p){
  struct thread* th = 0;
  
  for(int i = 1; i <= NTHREAD; ++i){
    int idx=tidx(p->recent-ttable+i);
    if(ttable[idx].state != RUNNABLE || ttable[idx].group != p)
      continue;
    th = ttable+idx;
    p->recent = th;
    break;
  }

  if(th == 0)
    return -1;

  ++p->tot_time,++p->queue_time;
  c->proc = p;
  //c->thread = th;
  switchuvm(p);
  th->state = RUNNING;
  swtch(&(c->scheduler), th->context);
  switchkvm();
  c->proc = 0;
  //c->thread = 0;

  return 0;
}

int 
next_qidx(int idx)
{
  return (idx+1)%(NPROC+1);
}

int before_qidx(int idx)
{
  return (idx+NPROC)%(NPROC+1);
}

void 
q_push(struct Queue* q, struct proc* p)
{
  if(q->sz >= NPROC)
    panic("queue overflow\n");
  q->arr[q->back] = p;
  ++q->sz;
  q->back = next_qidx(q->back);
}

struct proc* 
q_pop(struct Queue* q)
{
  if(!q->sz)
    panic("deletion from empty queue\n");
  struct proc* ret = q->arr[q->front];
  q->arr[q->front] = 0;
  --q->sz;
  q->front = next_qidx(q->front);
  return ret;
}
struct proc* 
q_pop_idx(struct Queue* q, int idx)
{
  if(!q->sz)
    panic("deletion from empty queue\n");
  if(q->front < q->back && (idx < q->front || idx >= q->back))
    panic("deletion from invalid index\n");
  if(q->front >= q->back && (idx < q->front && idx >= q->back))
    panic("deletion from invalid index\n");
  struct proc* ret = q->arr[idx];
  --q->sz;
  for(int i = next_qidx(idx);i != q->back; i = next_qidx(i))
    q->arr[before_qidx(i)] = q->arr[i];
  q->arr[q->back] = 0;
  q->back = before_qidx(q->back);
  return ret;
}

struct proc* 
q_front(struct Queue* q)
{
  if(!q->sz)
    return 0;
  return q->arr[q->front];
}

void
stride_push(struct proc* p)
{
  if(stride.sz > NPROC)
    panic("stride overflow\n");
  int idx = stride.sz++;
  while(idx && stride.arr[idx-1]->pass > p->pass){
    stride.arr[idx] = stride.arr[idx-1];
    --idx;
  }
  stride.arr[idx] = p;
}

struct proc*
stride_pop()
{
  if(!stride.sz)
    panic("deletion from empty stride\n");
  struct proc* ret = stride.arr[0];
  for(int i = 1; i<stride.sz; ++i)
    stride.arr[i-1] = stride.arr[i];
  stride.arr[stride.sz-1] = 0;
  --stride.sz;
  return ret;
}

struct proc* 
stride_pop_idx(int idx)
{
  if(idx >= stride.sz)
    panic("deletion from invalid index\n");
  struct proc* ret = stride.arr[idx];
  for(int i = idx+1; i < stride.sz; ++i)
    stride.arr[i-1] = stride.arr[i];
  stride.arr[--stride.sz] = 0;
  return ret;
}

struct proc* 
stride_top()
{
  if(!stride.sz)
    panic("finding from empty stride\n");
  return stride.arr[0];
}

void 
boost(){
  struct proc* p;
  for(int i = 0; i < 3; ++i){
    int cnt=mlfq.q[i].sz;
    while(cnt--){
      p = q_pop(mlfq.q + i);
      p->tot_time = p->queue_time = p->level = 0;
      q_push(mlfq.q, p);
    }
  }
}

void
MLFQ_schedule(struct cpu* c)
{
  ++mlfq.tick, ++mlfq_manager.tot_time;
  struct proc* p;
  for(int i = 0; i < 3; ++i){
    int cnt = mlfq.q[i].sz;
    while(cnt--){
      if((p = q_front(mlfq.q+i))->state != RUNNABLE || proceed(c, p) < 0){
        p->queue_time = 0;
        q_pop(mlfq.q+i);
        q_push(mlfq.q+i, p);
        continue;
      }

      if(p != q_front(mlfq.q+i))
        goto done;

      if(i < 2 && p->tot_time >= LOWER_TICK(i)){
        p->tot_time = p->queue_time = 0;
        ++p->level;
        q_pop(mlfq.q+i);
        q_push(mlfq.q+i+1, p);
      }
      else{
        p->queue_time = 0;
        q_pop(mlfq.q+i);
        q_push(mlfq.q+i, p);
      }
      goto done;
    }
  }

done:
  if(mlfq.tick >= BOOST_TICK){
    mlfq.tick = mlfq_manager.tot_time = 0;
    boost();
  }
}

void 
reinitiate(){
  for(int i = 0; i<stride.sz;++i)
    stride.arr[i]->pass = 0;
}

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  //initlock(&ttable.lock, "ttable");
  mlfq_manager.isDummy = 1;
  mlfq_manager.level = -1;
  mlfq_manager.ticket = NTICKET;
  mlfq_manager.stride = NSHARE / mlfq_manager.ticket;
  mlfq_manager.pass = 0;
  mlfq_manager.tot_time = 0;
  mlfq_manager.queue_time = 0;
  mlfq.tick = 0;
  for(int i = 0; i < 3; ++i){
    mlfq.q[i].sz = 0;
    mlfq.q[i].front = 0;
    mlfq.q[i].back = 0;
  }
  stride_push(&mlfq_manager);
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) 
{
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

struct thread*
mythread(void)
{
  struct proc* p = myproc();
  if(p == 0)
    return 0;
  return p->recent;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc* p;
  struct thread* th;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found1;

  release(&ptable.lock);
  return 0;

found1:
  for(th = ttable_begin(); th < ttable_end(); ++th)
    if(th->state == UNUSED && th->group == 0)
      goto found2;

  release(&ptable.lock);
  return 0;
  
found2:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->recent = th;

  // Allocate kernel stack.
  if((p->recent->kstack = kalloc()) == 0){
    p->state = UNUSED;
    p->pid = 0;
    p->recent = 0;
    return 0;
  }

  sp = p->recent->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *(p->recent->tf);
  (p->recent->tf) = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *(p->recent->context);
  (p->recent->context) = (struct context*)sp;
  memset(p->recent->context, 0, sizeof *(p->recent->context));
  (p->recent->context)->eip = (uint)forkret;

  p->recent->state = EMBRYO;
  p->recent->group = p;
  p->recent->retval = 0;
  p->recent->tid = nexttid++;
  p->recent->chan = 0;
  if(p == &mlfq_manager)
    panic("MLFQ alloc\n");
  p->isDummy = 0;
  p->level = 0;
  p->ticket = 0;
  p->stride = 0;
  p->pass = 0;
  p->tot_time = 0;
  p->queue_time = 0;
  q_push(mlfq.q, p);
  release(&ptable.lock);

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->recent->tf, 0, sizeof(*(p->recent->tf)));
  p->recent->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->recent->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->recent->tf->es = p->recent->tf->ds;
  p->recent->tf->ss = p->recent->tf->ds;
  p->recent->tf->eflags = FL_IF;
  p->recent->tf->esp = PGSIZE;
  p->recent->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  p->recent->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0){
      release(&ptable.lock);
      return -1;
    }
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0){
      release(&ptable.lock);
      return -1;
    }
  }
  curproc->sz = sz;
  switchuvm(curproc);
  release(&ptable.lock);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc* np;
  struct proc* curproc = myproc();
  struct thread* curthread = mythread();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  acquire(&ptable.lock);
  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->recent->kstack);
    np->recent->kstack = 0;
    np->state = UNUSED;
    np->recent->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *(np->recent->tf) = *(curthread->tf);

  // Clear %eax so that fork returns 0 in the child.
  np->recent->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  np->state = RUNNABLE;
  np->recent->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc* curproc = myproc();
  struct proc* p;
  struct thread* th;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  //sync();
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;

  for(th = ttable_begin(); th < ttable_end(); ++th)
    if(th->state != UNUSED && th->group == curproc)
      th->state = ZOMBIE;

  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc* p;
  struct thread* th;
  int havekids, pid;
  struct proc* curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        freevm(p->pgdir);
        //cprintf("freevm\n");
        for(th = ttable_begin(); th < ttable_end(); ++th){
          if(th->group == p){
            if(th->kstack != 0)
              kfree(th->kstack);
            th->kstack = 0;
            //cprintf("eliminating ustack in tid %d\n",th->tid);
            th->ustack = 0;
            th->chan = 0;
            th->context = 0;
            th->group = 0;
            th->retval = 0;
            th->state = UNUSED;
            th->tf = 0;
            th->tid = 0;
          }
        }
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;

        if(p->ticket){
          mlfq_manager.ticket += p->ticket;
          mlfq_manager.stride = NSHARE/p->ticket;
        }
        if(p == &mlfq_manager)
          panic("MLFQ wait\n");
        p->isDummy = 0;
        p->level = 0;
        p->ticket = 0;
        p->stride = 0;
        p->pass = 0;
        p->tot_time = 0;
        p->queue_time = 0;
        p->state = UNUSED;
        for(int i = 0; i < 3; ++i){
          for(int j = mlfq.q[i].front; j != mlfq.q[i].back; j = next_qidx(j)){
            if(mlfq.q[i].arr[j] == p){
              q_pop_idx(mlfq.q+i, j);
              //cprintf("MLFQ pop\n");
              goto found;
            }
          }
        }
        for(int i = 0; i < stride.sz; ++i){
          if(stride.arr[i] == p){
            stride_pop_idx(i);
            //cprintf("stride pop\n");
            goto found;
          }
        }
        //cprintf("find failure\n");
      found:
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    struct proc* p = stride_top();
    if(p->pass > REINITIATE_TICK)
      reinitiate();
    if(p->isDummy){
      //cprintf("MLFQ\n");
      MLFQ_schedule(c);
      p->pass += p->stride;
      stride_pop();
      stride_push(p);
    }
    else{
      //cprintf("Stride\n");
      if(p->state != RUNNABLE || proceed(c, p) < 0);
      p->pass += p->stride;
      p->queue_time = 0;
      stride_pop();
      stride_push(p);
    }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  //struct proc *p = myproc();
  struct thread* th = mythread();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(th->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&th->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

void
proc_yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  mythread()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

void
thread_yield(struct thread* th)
{
  struct proc* p = myproc();
  struct thread* before = mythread();
  int intena;
  acquire(&ptable.lock); 
  before->state = RUNNABLE;
  p->recent = th;
  mycpu()->ts.esp0 = (uint)p->recent->kstack + KSTACKSIZE;
  th->state= RUNNING;
  intena = mycpu()->intena;
  swtch(&(before->context),th->context);
  mycpu()->intena = intena;
  release(&ptable.lock);
}

int 
check(struct thread** pth)
{
  struct proc* p = myproc();
  struct thread* th;
  if(p->state != RUNNABLE)
    return -1;
  if(p->level >= 0 && p->queue_time >= QUEUE_TICK(p->level))
    return -1;
  if(p->level < 0 && p->queue_time >= STRIDE_TICK)
    return -1;
  acquire(&ptable.lock);
  for(int i = 1; i <= NTHREAD; ++i){
    int idx = tidx((p->recent - ttable) + i);
    th = ttable+idx;
    if(th->state == RUNNABLE && th->group == p){
      *pth = th;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct thread* th;
  if(check(&th))
    proc_yield();
  else 
    thread_yield(th);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc* p = myproc();
  struct thread* th = mythread();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  th->chan = chan;
  th->state = SLEEPING;

  sched();

  // Tidy up.
  th->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc* p;
  struct thread* th;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state != UNUSED)
      for(th = ttable_begin(); th < ttable_end(); ++th)
        if(th->state == SLEEPING && th->chan == chan)
          th->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

int
set_cpu_share(int share)
{
  struct proc* p = myproc();
  //cprintf("initial share: %d\n",p->ticket);
  //cprintf("initial pass: %d %d\n",p->pass,stride_top()->pass);
  acquire(&ptable.lock);
  if(mlfq_manager.ticket - (share - p->ticket) < MIN_CPU_SHARE){
    release(&ptable.lock);
    return -1;
  }
  if(!p->ticket)
    mlfq_manager.ticket -= share;
  else
    mlfq_manager.ticket -= share-p->ticket;
  mlfq_manager.stride = NSHARE/mlfq_manager.ticket;
  //cprintf("tickets: %d\n",mlfq_manager.ticket);
  for(int i = 0; i < 3; ++i){
    for(int j = mlfq.q[p->level].front; j != mlfq.q[p->level].back; j = next_qidx(j)){
      if(mlfq.q[i].arr[j] == p){
        //cprintf("pop: %d %p\n",i,p);
        q_pop_idx(mlfq.q+i, j);
        goto found;
      }
    }
  }
  p->ticket = share;
  p->stride = NSHARE/p->ticket;
  //cprintf("new share: %d\n",p->ticket);
  release(&ptable.lock);
  return 0; // already in stride
found:
  p->tot_time = 0;
  p->queue_time = 0;
  p->ticket = share;
  p->stride = NSHARE/p->ticket;
  p->level = -1;
  if(!stride.sz)
    p->pass = 0;
  else
    p->pass = stride_top()->pass;
  stride_push(p);
  //cprintf("new share: %d\n",p->ticket);
  //cprintf("new pass: %d %d\n",p->pass,stride_top()->pass);
  release(&ptable.lock);
  return 0;
}

int 
getlev(void)
{
  return myproc()->level;
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc* p;
  struct thread* th;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake thread from sleep if necessary.
      for(th = ttable_begin(); th < ttable_end(); ++th)
        if(th->state == SLEEPING && th->group == p)
          th->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->recent->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
