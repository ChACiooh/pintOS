#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/fixed_point.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

#define NICE_DEFAULT	0
#define RECENT_CPU_DEFAULT	0
#define LOAD_AVG_DEFAULT	0
#define NICE_MIN	-20
#define NICE_MAX	20

int load_avg;

/* List for saving threads blocked. */
static struct list sleep_list;

/* Min-value among values of wakeup_ticks in sleep_list. */
int64_t next_tick_to_awake = INT64_MAX;

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  /* Initialize sleep_list. */
  list_init(&sleep_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);
  load_avg = LOAD_AVG_DEFAULT;	// when thread is created, initialize.

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);

  t->parent = thread_current();  //Save parent process
  t->priority = priority;		//Save priority
  // t->recent_cpu = t->parent->recent_cpu;
  t->is_exit = false;    //program not exit
  t->load_status = -1;  //program not load
  sema_init(&(t->s_load), 0);  //load semaphore init
  sema_init(&(t->s_exit), 0);  //exit seamphore init
  list_push_back(&(t->parent->child_list), &(t->child_elem));  //add child list
  
   /* init File Descriptor */
  t->file_descriptor = palloc_get_page(0);
  t->max_fd = 2;
  
  /* Add to run queue. */
  thread_unblock (t);

  /* Yield cpu if created thread's priority is higher than current thread's. */
  if( t->priority > t->parent->priority )
  {
	  thread_yield();
  }

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);

  /* Insert element(thread) into ready_list along the priority. */
  list_insert_ordered(&ready_list, &t->elem, cmp_priority, NULL);
  
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  struct thread* t;
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  t = thread_current();
  list_remove (&t->allelem);
  
  t->is_exit = true;
  if(strcmp(t->name,"main"))	//if Thread is Main Thread -> NO SEMA_UP
  {
    sema_up(&(t->s_exit));
  }
  
  t->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
  {
	  // list_push_back (&ready_list, &cur->elem);
	  list_insert_ordered(&ready_list, &cur->elem, cmp_priority, NULL);
  }

  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Set the current thread's priority to NEW_PRIORITY. 
 * And need to consider donation and if it is mlfq. */
void
thread_set_priority (int new_priority) 
{
	if(thread_mlfqs)	return;
	enum intr_level old_level = intr_disable ();
	struct thread *cur = thread_current();
	int old_priority = cur->priority;
	thread_current()->init_priority = new_priority;	// set new priority on current thread.
  	refresh_priority();	// refresh donation data.

	if(old_priority < cur->priority)	// became upper!
  		donate_priority();
	else if(old_priority > cur->priority)	// became under...
  		test_max_priority();	// scheduling. pass my turn.

	intr_set_level(old_level);
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
	enum intr_level old_level = intr_disable ();

	/* boundary check. */
	if(nice < NICE_MIN)	nice = NICE_MIN;
	else if(nice > NICE_MAX)	nice = NICE_MAX;

	/* Change current thread's nice value.
	 * After changing, recalculate the priority
	 * of current thread and schedule.
	 */
	thread_current()->nice = nice;
	mlfqs_priority(thread_current());
	test_max_priority();
	intr_set_level(old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  enum intr_level old_level = intr_disable ();
  int return_val = thread_current()->nice;
  intr_set_level(old_level);
  return return_val;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  enum intr_level old_level = intr_disable ();
  int return_val = fp_to_int_round(mult_mixed(load_avg, 100));
  intr_set_level(old_level);
  return return_val;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  enum intr_level old_level = intr_disable ();
  int return_val = fp_to_int_round(mult_mixed(thread_current()->recent_cpu, 100));
  intr_set_level(old_level);
  return return_val;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;
  
  /* initialize data structures about priority-donation. */
  t->init_priority = priority;
  t->wait_on_lock = NULL;
  t->nice = NICE_DEFAULT;
  t->recent_cpu = RECENT_CPU_DEFAULT;
  list_init(&t->donations);

  /* push the thread in the total list. */
  list_push_back (&all_list, &t->allelem);
  /* init List of child */
  list_init(&(t->child_list));
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

void thread_sleep(int64_t ticks)
{
	struct thread *cur = thread_current();
	if(cur == idle_thread)	return;

	//cur->status = THREAD_BLOCKED;
	enum intr_level old_level = intr_disable();
	cur->wakeup_tick = ticks;	// Tick to wake up.
	list_push_back(&sleep_list, &cur->elem);	// Insert current thread into sleep_list.
	update_next_tick_to_awake(ticks);
	thread_block();	// Make current thread blocked and schedule.
	intr_set_level(old_level);
}

void thread_awake(int64_t ticks)	// This ticks means tick to awake.
{
	struct list_elem *e;
	struct thread *t;
	for (e = list_begin (&sleep_list); e != list_end (&sleep_list); )
    {
		t = list_entry (e, struct thread, elem);

		if(t->wakeup_tick <= ticks)
		{
			e = list_remove(e);	// Now, e points next pointer before remove.
			thread_unblock(t);
		}
		else
		{
			update_next_tick_to_awake(ticks);
			e = list_next(e);
		}
    }
	// update_next_tick_to_awake(ticks);
}

void update_next_tick_to_awake(int64_t ticks)
{
	if(ticks <= next_tick_to_awake)
		next_tick_to_awake = ticks;
}

int64_t get_next_tick_to_awake(void)
{
	return next_tick_to_awake;
}

void test_max_priority (void)
{
	// If ready_list is empty, it doesn't work.
	if (list_empty(&ready_list))	return;
	
	struct thread *t = list_entry(list_begin(&ready_list), struct thread, elem);
	if(thread_current() != idle_thread && t->priority > thread_current()->priority)
	{
		thread_yield();
	}
}

bool cmp_priority(const struct list_elem* a_, const struct list_elem* b_, void* aux UNUSED)
{
	//printf("cmp_prio\n");
	const int a_priority = (list_entry(a_, struct thread, elem))->priority;
	const int b_priority = (list_entry(b_, struct thread, elem))->priority;
	return a_priority > b_priority;
}

/* functions for donation. */
void donate_priority(void)
{
	/* priority donation */
	struct thread *cur = thread_current();
	struct thread *next_asker;
	unsigned nested_depth = 0;	// for depth < 8.
	/* Because this functions will be called where current
	 * thread has waiters on the lock at least one, so you
	 * don't need to concern of null-ptr exception. */
	
	if(!cur->wait_on_lock)	return;	// exception dealing.
	for(next_asker = cur->wait_on_lock->holder;
			next_asker && nested_depth < 8;	++nested_depth) {
		if(next_asker->priority > cur->priority)	break;
		next_asker->priority = cur->priority;	// donation
		cur = next_asker;
		next_asker = (next_asker->wait_on_lock != NULL) ? 
						next_asker->wait_on_lock->holder : NULL;
	}
}

/* remove the entries that asked for the lock. 
 * from current thread's donation list. */
void remove_with_lock(struct lock *lock)
{
	struct thread *cur, *donator;
	struct list_elem *e;

	cur = thread_current();
	for(e = list_begin(&cur->donations); e != list_end(&cur->donations); )
	{
		donator = list_entry(e, struct thread, donation_elem);
		if(lock == donator->wait_on_lock){	
			// if now pointing wait_on_lock is same with lock, remove the element.
			e = list_remove(e);
		}
		else	e = list_next(e);
	}	// iteration end.
}

void refresh_priority(void)
{
	struct thread *cur = thread_current();
	struct thread *donator;
	struct list_elem *e;

	cur->priority = cur->init_priority;	// change present priority into initial priority.
	for(e = list_begin(&cur->donations); e != list_end(&cur->donations); e = list_next(e))
	{
		donator = list_entry(e, struct thread, donation_elem);
		if(donator->priority > cur->priority){
			/* set the priority of current thread as
			 * maximum priority among the donation 
			 * list of current thread. */
			cur->priority = donator->priority;			
		}
	}	// iteration end.
}

/* mlfqs implements start */
void mlfqs_priority (struct thread *t)
{
	/* calculate "t"'s priority. */
	if(t == idle_thread)	return;
	// priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)
	int max_priority = int_to_fp(PRI_MAX);
	int quad_r_cpu = div_mixed(t->recent_cpu, 4);
	int temp = sub_fp(max_priority, quad_r_cpu);
	int double_nice = t->nice * 2;
	temp = sub_mixed(temp, double_nice);
	t->priority = fp_to_int(temp);

	//t->priority = fp_to_int(sub_mixed(sub_fp(int_to_fp(PRI_MAX), div_mixed(t->recent_cpu, 4)),
	//						t->nice*2));
	if(t->priority < PRI_MIN)	t->priority = PRI_MIN;
	else if(t->priority > PRI_MAX)	t->priority = PRI_MAX;
}

void mlfqs_recent_cpu (struct thread *t)
{
	/* calculate "t"'s recent cpu. */
	if(t == idle_thread)	return;
	// recent_cpu = (2*load_avg)/(2*load_avg + 1)*recent_cpu + nice
	
	int double_l_avg = mult_mixed(load_avg, 2);
	int temp = add_mixed(double_l_avg, 1);
	temp = div_fp(double_l_avg, temp);
	temp = mult_fp(temp, t->recent_cpu);
	t->recent_cpu = add_mixed(temp, t->nice);
}

void mlfqs_load_avg (void)
{
	/* calculate load average. */
	// load_avg = (59/60)*load_avg+(1/60)*ready_threads
	
	int ready_threads = list_size(&ready_list);
	// If current thread is not idle, +1.
	if(thread_current() != idle_thread)
		++ready_threads;
	
	int temp = div_mixed(int_to_fp(59), 60);
	temp = mult_fp(temp, load_avg);
	ready_threads = div_mixed(int_to_fp(ready_threads), 60);
	load_avg = add_fp(temp, ready_threads);

	ASSERT(load_avg >= 0);	// it CANNOT be negative number.
}

void mlfqs_increment (void)	// add 1 to recent cpu of current cpu
{
	if(thread_current() == idle_thread)	return;
	thread_current()->recent_cpu = add_mixed(thread_current()->recent_cpu, 1);
}

void mlfqs_recalc (void)
{
	/* recalculate resource of all of threads. */
	struct list_elem *e;
	struct thread *t;
	for(e = list_begin(&all_list); e != list_end(&all_list);
			e = list_next(e))
	{
		t = list_entry(e, struct thread, allelem);	// ALL OF THREADS
		mlfqs_priority(t);
		mlfqs_recent_cpu(t);
	}
}
