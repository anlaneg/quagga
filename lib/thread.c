/* Thread management routine
 * Copyright (C) 1998, 2000 Kunihiro Ishiguro <kunihiro@zebra.org>
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.  
 */

/* #define DEBUG */

#include <zebra.h>
#include <sys/resource.h>

#include "thread.h"
#include "memory.h"
#include "log.h"
#include "hash.h"
#include "pqueue.h"
#include "command.h"
#include "sigevent.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

/* Recent absolute time of day */
struct timeval recent_time;
static struct timeval last_recent_time;
/* Relative time, since startup */
static struct timeval relative_time;
static struct timeval relative_time_base;
/* init flag */
static unsigned short timers_inited;

static struct hash *cpu_record = NULL;

/* Struct timeval's tv_usec one second value.  */
#define TIMER_SECOND_MICRO 1000000L

/* Adjust so that tv_usec is in the range [0,TIMER_SECOND_MICRO).
   And change negative values to 0. */
static struct timeval
timeval_adjust (struct timeval a)
{
  while (a.tv_usec >= TIMER_SECOND_MICRO)
    {
      a.tv_usec -= TIMER_SECOND_MICRO;
      a.tv_sec++;
    }

  while (a.tv_usec < 0)
    {
      a.tv_usec += TIMER_SECOND_MICRO;
      a.tv_sec--;
    }

  if (a.tv_sec < 0)
      /* Change negative timeouts to 0. */
      a.tv_sec = a.tv_usec = 0;

  return a;
}

static struct timeval
timeval_subtract (struct timeval a, struct timeval b)
{
  struct timeval ret;

  ret.tv_usec = a.tv_usec - b.tv_usec;
  ret.tv_sec = a.tv_sec - b.tv_sec;

  return timeval_adjust (ret);
}

static long
timeval_cmp (struct timeval a, struct timeval b)
{
  return (a.tv_sec == b.tv_sec
	  ? a.tv_usec - b.tv_usec : a.tv_sec - b.tv_sec);
}

//获得两个时间的差
unsigned long
timeval_elapsed (struct timeval a, struct timeval b)
{
  return (((a.tv_sec - b.tv_sec) * TIMER_SECOND_MICRO)
	  + (a.tv_usec - b.tv_usec));
}

#if !defined(HAVE_CLOCK_MONOTONIC) && !defined(__APPLE__)
static void
quagga_gettimeofday_relative_adjust (void)
{
  struct timeval diff;
  if (timeval_cmp (recent_time, last_recent_time) < 0)
    {
      relative_time.tv_sec++;
      relative_time.tv_usec = 0;
    }
  else
    {
      diff = timeval_subtract (recent_time, last_recent_time);
      relative_time.tv_sec += diff.tv_sec;
      relative_time.tv_usec += diff.tv_usec;
      relative_time = timeval_adjust (relative_time);
    }
  last_recent_time = recent_time;
}
#endif /* !HAVE_CLOCK_MONOTONIC && !__APPLE__ */

/* gettimeofday wrapper, to keep recent_time updated */
static int
quagga_gettimeofday (struct timeval *tv)
{
  int ret;
  
  assert (tv);
  
  if (!(ret = gettimeofday (&recent_time, NULL)))
    {
      /* init... */
      if (!timers_inited)
        {
          relative_time_base = last_recent_time = recent_time;
          timers_inited = 1;
        }
      /* avoid copy if user passed recent_time pointer.. */
      if (tv != &recent_time)
        *tv = recent_time;
      return 0;
    }
  return ret;
}

static int
quagga_get_relative (struct timeval *tv)
{
  int ret;

#ifdef HAVE_CLOCK_MONOTONIC
  {
    struct timespec tp;
    if (!(ret = clock_gettime (CLOCK_MONOTONIC, &tp)))
      {
    	//更新relative_time,使其等于当前时间
        relative_time.tv_sec = tp.tv_sec;
        relative_time.tv_usec = tp.tv_nsec / 1000;
      }
  }
#elif defined(__APPLE__)
  {
    uint64_t ticks;
    uint64_t useconds;
    static mach_timebase_info_data_t timebase_info;

    ticks = mach_absolute_time();
    if (timebase_info.denom == 0)
      mach_timebase_info(&timebase_info);

    useconds = ticks * timebase_info.numer / timebase_info.denom / 1000;
    relative_time.tv_sec = useconds / 1000000;
    relative_time.tv_usec = useconds % 1000000;

    return 0;
  }
#else /* !HAVE_CLOCK_MONOTONIC && !__APPLE__ */
  if (!(ret = quagga_gettimeofday (&recent_time)))
    quagga_gettimeofday_relative_adjust();
#endif /* HAVE_CLOCK_MONOTONIC */

  if (tv)
    *tv = relative_time;

  return ret;
}

/* Get absolute time stamp, but in terms of the internal timer
 * Could be wrong, but at least won't go back.
 */
static void
quagga_real_stabilised (struct timeval *tv)
{
  *tv = relative_time_base;
  tv->tv_sec += relative_time.tv_sec;
  tv->tv_usec += relative_time.tv_usec;
  *tv = timeval_adjust (*tv);
}

/* Exported Quagga timestamp function.
 * Modelled on POSIX clock_gettime.
 */
int
quagga_gettime (enum quagga_clkid clkid, struct timeval *tv)
{
  switch (clkid)
    {
      case QUAGGA_CLK_REALTIME:
        return quagga_gettimeofday (tv);
      case QUAGGA_CLK_MONOTONIC:
        return quagga_get_relative (tv);
      case QUAGGA_CLK_REALTIME_STABILISED:
        quagga_real_stabilised (tv);
        return 0;
      default:
        errno = EINVAL;
        return -1;
    }
}

/* time_t value in terms of stabilised absolute time. 
 * replacement for POSIX time()
 */
time_t
quagga_time (time_t *t)
{
  struct timeval tv;
  quagga_real_stabilised (&tv);
  if (t)
    *t = tv.tv_sec;
  return tv.tv_sec;
}

/* Public export of recent_relative_time by value */
struct timeval
recent_relative_time (void)
{
  return relative_time;
}

static unsigned int
cpu_record_hash_key (struct cpu_thread_history *a)
{
  return (uintptr_t) a->func;
}

static int 
cpu_record_hash_cmp (const struct cpu_thread_history *a,
		     const struct cpu_thread_history *b)
{
  return a->func == b->func;
}

static void *
cpu_record_hash_alloc (struct cpu_thread_history *a)
{
  struct cpu_thread_history *new;
  new = XCALLOC (MTYPE_THREAD_STATS, sizeof (struct cpu_thread_history));
  new->func = a->func;
  new->funcname = a->funcname;
  return new;
}

static void
cpu_record_hash_free (void *a)
{
  struct cpu_thread_history *hist = a;
 
  XFREE (MTYPE_THREAD_STATS, hist);
}

static void 
vty_out_cpu_thread_history(struct vty* vty,
			   struct cpu_thread_history *a)
{
#ifdef HAVE_RUSAGE
  vty_out(vty, "%7ld.%03ld %9d %8ld %9ld %8ld %9ld",
	  a->cpu.total/1000, a->cpu.total%1000, a->total_calls,
	  a->cpu.total/a->total_calls, a->cpu.max,
	  a->real.total/a->total_calls, a->real.max);
#else
  vty_out(vty, "%7ld.%03ld %9d %8ld %9ld",
	  a->real.total/1000, a->real.total%1000, a->total_calls,
	  a->real.total/a->total_calls, a->real.max);
#endif
  vty_out(vty, " %c%c%c%c%c%c %s%s",
	  a->types & (1 << THREAD_READ) ? 'R':' ',
	  a->types & (1 << THREAD_WRITE) ? 'W':' ',
	  a->types & (1 << THREAD_TIMER) ? 'T':' ',
	  a->types & (1 << THREAD_EVENT) ? 'E':' ',
	  a->types & (1 << THREAD_EXECUTE) ? 'X':' ',
	  a->types & (1 << THREAD_BACKGROUND) ? 'B' : ' ',
	  a->funcname, VTY_NEWLINE);
}

static void
cpu_record_hash_print(struct hash_backet *bucket, 
		      void *args[])
{
  struct cpu_thread_history *totals = args[0];
  struct vty *vty = args[1];
  thread_type *filter = args[2];
  struct cpu_thread_history *a = bucket->data;
  
  a = bucket->data;
  if ( !(a->types & *filter) )
       return;
  vty_out_cpu_thread_history(vty,a);
  totals->total_calls += a->total_calls;
  totals->real.total += a->real.total;
  if (totals->real.max < a->real.max)
    totals->real.max = a->real.max;
#ifdef HAVE_RUSAGE
  totals->cpu.total += a->cpu.total;
  if (totals->cpu.max < a->cpu.max)
    totals->cpu.max = a->cpu.max;
#endif
}

static void
cpu_record_print(struct vty *vty, thread_type filter)
{
  struct cpu_thread_history tmp;
  void *args[3] = {&tmp, vty, &filter};

  memset(&tmp, 0, sizeof tmp);
  tmp.funcname = "TOTAL";
  tmp.types = filter;

#ifdef HAVE_RUSAGE
  vty_out(vty, "%21s %18s %18s%s",
  	  "", "CPU (user+system):", "Real (wall-clock):", VTY_NEWLINE);
#endif
  vty_out(vty, "Runtime(ms)   Invoked Avg uSec Max uSecs");
#ifdef HAVE_RUSAGE
  vty_out(vty, " Avg uSec Max uSecs");
#endif
  vty_out(vty, "  Type  Thread%s", VTY_NEWLINE);
  hash_iterate(cpu_record,
	       (void(*)(struct hash_backet*,void*))cpu_record_hash_print,
	       args);

  if (tmp.total_calls > 0)
    vty_out_cpu_thread_history(vty, &tmp);
}

DEFUN(show_thread_cpu,
      show_thread_cpu_cmd,
      "show thread cpu [FILTER]",
      SHOW_STR
      "Thread information\n"
      "Thread CPU usage\n"
      "Display filter (rwtexb)\n")
{
  int i = 0;
  thread_type filter = (thread_type) -1U;

  if (argc > 0)
    {
      filter = 0;
      while (argv[0][i] != '\0')
	{
	  switch ( argv[0][i] )
	    {
	    case 'r':
	    case 'R':
	      filter |= (1 << THREAD_READ);
	      break;
	    case 'w':
	    case 'W':
	      filter |= (1 << THREAD_WRITE);
	      break;
	    case 't':
	    case 'T':
	      filter |= (1 << THREAD_TIMER);
	      break;
	    case 'e':
	    case 'E':
	      filter |= (1 << THREAD_EVENT);
	      break;
	    case 'x':
	    case 'X':
	      filter |= (1 << THREAD_EXECUTE);
	      break;
	    case 'b':
	    case 'B':
	      filter |= (1 << THREAD_BACKGROUND);
	      break;
	    default:
	      break;
	    }
	  ++i;
	}
      if (filter == 0)
	{
	  vty_out(vty, "Invalid filter \"%s\" specified,"
                  " must contain at least one of 'RWTEXB'%s",
		  argv[0], VTY_NEWLINE);
	  return CMD_WARNING;
	}
    }

  cpu_record_print(vty, filter);
  return CMD_SUCCESS;
}

static void
cpu_record_hash_clear (struct hash_backet *bucket, 
		      void *args)
{
  thread_type *filter = args;
  struct cpu_thread_history *a = bucket->data;
  
  a = bucket->data;
  if ( !(a->types & *filter) )
       return;
  
  hash_release (cpu_record, bucket->data);
}

static void
cpu_record_clear (thread_type filter)
{
  thread_type *tmp = &filter;
  hash_iterate (cpu_record,
	        (void (*) (struct hash_backet*,void*)) cpu_record_hash_clear,
	        tmp);
}

DEFUN(clear_thread_cpu,
      clear_thread_cpu_cmd,
      "clear thread cpu [FILTER]",
      "Clear stored data\n"
      "Thread information\n"
      "Thread CPU usage\n"
      "Display filter (rwtexb)\n")
{
  int i = 0;
  thread_type filter = (thread_type) -1U;

  if (argc > 0)
    {
      filter = 0;
      while (argv[0][i] != '\0')
	{
	  switch ( argv[0][i] )
	    {
	    case 'r':
	    case 'R':
	      filter |= (1 << THREAD_READ);
	      break;
	    case 'w':
	    case 'W':
	      filter |= (1 << THREAD_WRITE);
	      break;
	    case 't':
	    case 'T':
	      filter |= (1 << THREAD_TIMER);
	      break;
	    case 'e':
	    case 'E':
	      filter |= (1 << THREAD_EVENT);
	      break;
	    case 'x':
	    case 'X':
	      filter |= (1 << THREAD_EXECUTE);
	      break;
	    case 'b':
	    case 'B':
	      filter |= (1 << THREAD_BACKGROUND);
	      break;
	    default:
	      break;
	    }
	  ++i;
	}
      if (filter == 0)
	{
	  vty_out(vty, "Invalid filter \"%s\" specified,"
                  " must contain at least one of 'RWTEXB'%s",
		  argv[0], VTY_NEWLINE);
	  return CMD_WARNING;
	}
    }

  cpu_record_clear (filter);
  return CMD_SUCCESS;
}

static int
thread_timer_cmp(void *a, void *b)
{
  struct thread *thread_a = a;
  struct thread *thread_b = b;

  long cmp = timeval_cmp(thread_a->u.sands, thread_b->u.sands);

  if (cmp < 0)
    return -1;
  if (cmp > 0)
    return 1;
  return 0;
}

static void
thread_timer_update(void *node, int actual_position)
{
  struct thread *thread = node;

  thread->index = actual_position;
}

/* Allocate new thread master.  */
/*申请thread master*/
struct thread_master *
thread_master_create ()
{
  struct thread_master *rv;
  struct rlimit limit;

  getrlimit(RLIMIT_NOFILE, &limit);

  if (cpu_record == NULL) 
    cpu_record 
      = hash_create ((unsigned int (*) (void *))cpu_record_hash_key,
		     (int (*) (const void *, const void *))cpu_record_hash_cmp);

  rv = XCALLOC (MTYPE_THREAD_MASTER, sizeof (struct thread_master));
  if (rv == NULL)
    {
      return NULL;
    }

  rv->fd_limit = (int)limit.rlim_cur;
  rv->read = XCALLOC (MTYPE_THREAD, sizeof (struct thread *) * rv->fd_limit);
  if (rv->read == NULL)
    {
      XFREE (MTYPE_THREAD_MASTER, rv);
      return NULL;
    }

  rv->write = XCALLOC (MTYPE_THREAD, sizeof (struct thread *) * rv->fd_limit);
  if (rv->write == NULL)
    {
      XFREE (MTYPE_THREAD, rv->read);
      XFREE (MTYPE_THREAD_MASTER, rv);
      return NULL;
    }

  /* Initialize the timer queues */
  rv->timer = pqueue_create();
  rv->background = pqueue_create();
  rv->timer->cmp = rv->background->cmp = thread_timer_cmp;
  rv->timer->update = rv->background->update = thread_timer_update;

  return rv;
}

/* Add a new thread to the list.  */
static void
thread_list_add (struct thread_list *list, struct thread *thread)
{
  thread->next = NULL;
  thread->prev = list->tail;
  if (list->tail)
    list->tail->next = thread;
  else
    list->head = thread;
  list->tail = thread;
  list->count++;
}

/* Delete a thread from the list. */
static struct thread *
thread_list_delete (struct thread_list *list, struct thread *thread)
{
  if (thread->next)
    thread->next->prev = thread->prev;
  else
    list->tail = thread->prev;
  if (thread->prev)
    thread->prev->next = thread->next;
  else
    list->head = thread->next;
  thread->next = thread->prev = NULL;
  list->count--;
  return thread;
}

static void
thread_delete_fd (struct thread **thread_array, struct thread *thread)
{
  thread_array[thread->u.fd] = NULL;
}

static void
thread_add_fd (struct thread **thread_array, struct thread *thread)
{
  thread_array[thread->u.fd] = thread;
}

/* Move thread to unuse list. */
static void
thread_add_unuse (struct thread *thread)
{
  assert (thread);
  /* thread_execute uses dummy threads, allocated on its stack */
  if (thread->master == NULL)
    return;
  
  thread->type = THREAD_UNUSED;
  assert (thread->next == NULL);
  assert (thread->prev == NULL);
  thread_list_add (&thread->master->unuse, thread);
}

/* Free all unused thread. */
static void
thread_list_free (struct thread_master *m, struct thread_list *list)
{
  struct thread *t;
  struct thread *next;

  for (t = list->head; t; t = next)
    {
      next = t->next;
      XFREE (MTYPE_THREAD, t);
      list->count--;
      m->alloc--;
    }
}

static void
thread_array_free (struct thread_master *m, struct thread **thread_array)
{
  struct thread *t;
  int index;

  for (index = 0; index < m->fd_limit; ++index)
    {
      t = thread_array[index];
      if (t)
        {
          thread_array[index] = NULL;
          XFREE (MTYPE_THREAD, t);
          m->alloc--;
        }
    }
  XFREE (MTYPE_THREAD, thread_array);
}

static void
thread_queue_free (struct thread_master *m, struct pqueue *queue)
{
  int i;

  for (i = 0; i < queue->size; i++)
    XFREE(MTYPE_THREAD, queue->array[i]);

  m->alloc -= queue->size;
  pqueue_delete(queue);
}

/* Stop thread scheduler. */
void
thread_master_free (struct thread_master *m)
{
  thread_array_free (m, m->read);
  thread_array_free (m, m->write);
  thread_queue_free (m, m->timer);
  thread_list_free (m, &m->event);
  thread_list_free (m, &m->ready);
  thread_list_free (m, &m->unuse);
  thread_queue_free (m, m->background);
  
  XFREE (MTYPE_THREAD_MASTER, m);

  if (cpu_record)
    {
      hash_clean (cpu_record, cpu_record_hash_free);
      hash_free (cpu_record);
      cpu_record = NULL;
    }
}

/* Thread list is empty or not.  */
static int
thread_empty (struct thread_list *list)
{
    /*检查链表是否为空*/
  return  list->head ? 0 : 1;
}

/* Delete top of the list and return it. */
static struct thread *
thread_trim_head (struct thread_list *list)
{
	//自list中获取一个thread变量（头部获取）
  if (!thread_empty (list))
    return thread_list_delete (list, list->head);
  return NULL;
}

/* Return remain time in second. */
unsigned long
thread_timer_remain_second (struct thread *thread)
{
  quagga_get_relative (NULL);
  
  if (thread->u.sands.tv_sec - relative_time.tv_sec > 0)
    return thread->u.sands.tv_sec - relative_time.tv_sec;
  else
    return 0;
}

struct timeval
thread_timer_remain(struct thread *thread)
{
  quagga_get_relative(NULL);

  return timeval_subtract(thread->u.sands, relative_time);
}

//debug相关的参数定义
#define debugargdef  const char *funcname, const char *schedfrom, int fromln
#define debugargpass funcname, schedfrom, fromln

/* Get new thread.  */
//分配一个thread变量，用于扮演事件模式中的event
static struct thread *
thread_get (struct thread_master *m, u_char type,
	    int (*func) (struct thread *), void *arg, debugargdef)
{
  struct thread *thread = thread_trim_head (&m->unuse);

  if (! thread)
    {
      thread = XCALLOC (MTYPE_THREAD, sizeof (struct thread));
      m->alloc++;/*申请的thread总数增加*/
    }
  thread->type = type;
  thread->add_type = type;
  thread->master = m;
  thread->func = func;
  thread->arg = arg;
  thread->index = -1;

  thread->funcname = funcname;
  thread->schedfrom = schedfrom;
  thread->schedfrom_line = fromln;

  return thread;
}

#define fd_copy_fd_set(X) (X)

static int
fd_select (int size, thread_fd_set *read, thread_fd_set *write, thread_fd_set *except, struct timeval *t)
{
  return(select(size, read, write, except, t));
}

static int
fd_is_set (int fd, thread_fd_set *fdset)
{
  return FD_ISSET (fd, fdset);
}

static int
fd_clear_read_write (int fd, thread_fd_set *fdset)
{
  if (!FD_ISSET (fd, fdset))
    return 0;

  FD_CLR (fd, fdset);
  return 1;
}

//处理读写任务的创建
static struct thread *
funcname_thread_add_read_write (int dir/*事件类型*/, struct thread_master *m/*所属的master*/,
		 int (*func) (struct thread *), void *arg, int fd,
		 debugargdef)
{
  struct thread *thread = NULL;
  thread_fd_set *fdset = NULL;

  if (dir == THREAD_READ)
    fdset = &m->readfd;
  else
    fdset = &m->writefd;

  if (FD_ISSET (fd, fdset))
    {
	  //防止重复注册
      zlog (NULL, LOG_WARNING, "There is already %s fd [%d]",
	    (dir = THREAD_READ) ? "read" : "write", fd);
      return NULL;
    }

  //fd存入相应集合
  FD_SET (fd, fdset);

  thread = thread_get (m, dir, func, arg, debugargpass);
  thread->u.fd = fd;

  //将thread加入相应集合
  if (dir == THREAD_READ)
    thread_add_fd (m->read, thread);
  else
    thread_add_fd (m->write, thread);

  return thread;
}

/* Add new read thread. */
//添加一个读任务
struct thread *
funcname_thread_add_read (struct thread_master *m, 
		 int (*func) (struct thread *), void *arg, int fd,
		 debugargdef)
{
  return funcname_thread_add_read_write (THREAD_READ, m, func,
                                         arg, fd, debugargpass);
}

/* Add new write thread. */
//添加一个写任务
struct thread *
funcname_thread_add_write (struct thread_master *m,
		 int (*func) (struct thread *), void *arg, int fd,
		 debugargdef)
{
  return funcname_thread_add_read_write (THREAD_WRITE, m, func, 
                                         arg, fd, debugargpass);
}

//添加一个timer任务
static struct thread *
funcname_thread_add_timer_timeval (struct thread_master *m,
                                   int (*func) (struct thread *), 
                                  int type,
                                  void *arg, 
                                  struct timeval *time_relative,
				  debugargdef)
{
  struct thread *thread;
  struct pqueue *queue;
  struct timeval alarm_time;

  assert (m != NULL);

  assert (type == THREAD_TIMER || type == THREAD_BACKGROUND);
  assert (time_relative);
  
  queue = ((type == THREAD_TIMER) ? m->timer : m->background);
  thread = thread_get (m, type, func, arg, debugargpass);

  /* Do we need jitter here? */
  quagga_get_relative (NULL);
  alarm_time.tv_sec = relative_time.tv_sec + time_relative->tv_sec;
  alarm_time.tv_usec = relative_time.tv_usec + time_relative->tv_usec;
  thread->u.sands = timeval_adjust(alarm_time);

  pqueue_enqueue(thread, queue);
  return thread;
}


/* Add timer event thread. */
struct thread *
funcname_thread_add_timer (struct thread_master *m,
		           int (*func) (struct thread *), 
		           void *arg, long timer,
			   debugargdef)
{
  struct timeval trel;

  assert (m != NULL);

  trel.tv_sec = timer;
  trel.tv_usec = 0;

  return funcname_thread_add_timer_timeval (m, func, THREAD_TIMER, arg, 
                                            &trel, debugargpass);
}

/* Add timer event thread with "millisecond" resolution */
struct thread *
funcname_thread_add_timer_msec (struct thread_master *m,
                                int (*func) (struct thread *), 
                                void *arg, long timer,
				debugargdef)
{
  struct timeval trel;

  assert (m != NULL);

  trel.tv_sec = timer / 1000;
  trel.tv_usec = 1000*(timer % 1000);

  return funcname_thread_add_timer_timeval (m, func, THREAD_TIMER, 
                                            arg, &trel, debugargpass);
}

/* Add timer event thread with "millisecond" resolution */
struct thread *
funcname_thread_add_timer_tv (struct thread_master *m,
                              int (*func) (struct thread *),
                              void *arg, struct timeval *tv,
                              debugargdef)
{
  return funcname_thread_add_timer_timeval (m, func, THREAD_TIMER,
                                            arg, tv, debugargpass);
}

/* Add a background thread, with an optional millisec delay */
struct thread *
funcname_thread_add_background (struct thread_master *m,
                                int (*func) (struct thread *),
                                void *arg, long delay,
				debugargdef)
{
  struct timeval trel;
  
  assert (m != NULL);
  
  if (delay)
    {
      trel.tv_sec = delay / 1000;
      trel.tv_usec = 1000*(delay % 1000);
    }
  else
    {
      trel.tv_sec = 0;
      trel.tv_usec = 0;
    }

  return funcname_thread_add_timer_timeval (m, func, THREAD_BACKGROUND,
                                            arg, &trel, debugargpass);
}

/* Add simple event thread. */
struct thread *
funcname_thread_add_event (struct thread_master *m,
		  int (*func) (struct thread *), void *arg, int val,
		  debugargdef)
{
  struct thread *thread;

  assert (m != NULL);

  thread = thread_get (m, THREAD_EVENT, func, arg, debugargpass);
  thread->u.val = val;
  thread_list_add (&m->event, thread);

  return thread;
}

/* Cancel thread from scheduler. */
//取消一个任务
void
thread_cancel (struct thread *thread)
{
  struct thread_list *list = NULL;
  struct pqueue *queue = NULL;
  struct thread **thread_array = NULL;
  
  //按类型获取要查找的链
  switch (thread->type)
    {
    case THREAD_READ:
      assert (fd_clear_read_write (thread->u.fd, &thread->master->readfd));
      thread_array = thread->master->read;
      break;
    case THREAD_WRITE:
      assert (fd_clear_read_write (thread->u.fd, &thread->master->writefd));
      thread_array = thread->master->write;
      break;
    case THREAD_TIMER:
      queue = thread->master->timer;
      break;
    case THREAD_EVENT:
      list = &thread->master->event;
      break;
    case THREAD_READY:
      list = &thread->master->ready;
      break;
    case THREAD_BACKGROUND:
      queue = thread->master->background;
      break;
    default:
      return;
      break;
    }

  if (queue)
    {
	  //需要自queue中移除
      assert(thread->index >= 0);
      assert(thread == queue->array[thread->index]);
      pqueue_remove_at(thread->index, queue);
    }
  else if (list)
    {
	  //需要自链上移除
      thread_list_delete (list, thread);
    }
  else if (thread_array)
    {
	  //需要自thread_array上移除
      thread_delete_fd (thread_array, thread);
    }
  else
    {
      assert(!"Thread should be either in queue or list or array!");
    }

  thread_add_unuse (thread);
}

/* Delete all events which has argument value arg. */
unsigned int
thread_cancel_event (struct thread_master *m, void *arg)
{
  unsigned int ret = 0;
  struct thread *thread;

  thread = m->event.head;
  while (thread)
    {
      struct thread *t;

      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          ret++;
          thread_list_delete (&m->event, t);
          thread_add_unuse (t);
        }
    }

  /* thread can be on the ready list too */
  thread = m->ready.head;
  while (thread)
    {
      struct thread *t;

      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          ret++;
          thread_list_delete (&m->ready, t);
          thread_add_unuse (t);
        }
    }
  return ret;
}

static struct timeval *
thread_timer_wait (struct pqueue *queue, struct timeval *timer_val)
{
  if (queue->size)
    {
      struct thread *next_timer = queue->array[0];
      *timer_val = timeval_subtract (next_timer->u.sands, relative_time);
      return timer_val;
    }
  return NULL;
}

//检查thread对应的fd是否在fdset中，如果在，将其挂接在master->ready链上
static int
thread_process_fds_helper (struct thread_master *m, struct thread *thread, thread_fd_set *fdset)
{
  thread_fd_set *mfdset = NULL;
  struct thread **thread_array;

  if (!thread)
    return 0;

  if (thread->type == THREAD_READ)
    {
      mfdset = &m->readfd;
      thread_array = m->read;
    }
  else
    {
      mfdset = &m->writefd;
      thread_array = m->write;
    }

  if (fd_is_set (THREAD_FD (thread), fdset))
    {
	  //thread对应的fd在fdset中，将其自read/write fd数组中移除
      fd_clear_read_write (THREAD_FD (thread), mfdset);
      //将thead自对应的array中移除
      thread_delete_fd (thread_array, thread);
      //将其加入ready链
      thread_list_add (&m->ready, thread);
      thread->type = THREAD_READY;
      return 1;
    }
  return 0;
}

static int
thread_process_fds (struct thread_master *m, thread_fd_set *rset, thread_fd_set *wset, int num)
{
  int ready = 0, index;

  //遍历master所有需要read,write的thread,检查其相关的fd是否位于rset,wset中
  //如果在，将此thread存放在master->ready链上，准备触发
  for (index = 0; index < m->fd_limit && ready < num; ++index)
    {
      ready += thread_process_fds_helper (m, m->read[index], rset);
      ready += thread_process_fds_helper (m, m->write[index], wset);
    }
  return num - ready;
}

/* Add all timers that have popped to the ready list. */
static unsigned int
thread_timer_process (struct pqueue *queue, struct timeval *timenow)
{
  struct thread *thread;
  unsigned int ready = 0;
  
  while (queue->size)
    {
	  //获取最小堆上的第一个元素
      thread = queue->array[0];
      if (timeval_cmp (*timenow, thread->u.sands) < 0)
    	  //返回已出队的数目
        return ready;

      //timer已过期，将其自队列中移除
      pqueue_dequeue(queue);
      thread->type = THREAD_READY;
      //将timer添加至ready链上
      thread_list_add (&thread->master->ready, thread);
      ready++;
    }
  return ready;
}

/* process a list en masse, e.g. for event thread lists */
static unsigned int
thread_process (struct thread_list *list)
{
  //将list中的内容添加至master的ready链表
  struct thread *thread;
  struct thread *next;
  unsigned int ready = 0;
  
  for (thread = list->head; thread; thread = next)
    {
      next = thread->next;
      //由于是单链表，故需要自旧链上摘除掉
      thread_list_delete (list, thread);
      thread->type = THREAD_READY;
      thread_list_add (&thread->master->ready, thread);
      ready++;
    }
  return ready;
}

/* Fetch next ready thread. */
//获取一个待触发的任务
static struct thread *
thread_fetch (struct thread_master *m)
{
  struct thread *thread;
  thread_fd_set readfd;
  thread_fd_set writefd;
  thread_fd_set exceptfd;
  struct timeval timer_val = { .tv_sec = 0, .tv_usec = 0 };
  struct timeval timer_val_bg;
  struct timeval *timer_wait = &timer_val;
  struct timeval *timer_wait_bg;

  while (1)
    {
      int num = 0;

      /* Signals pre-empt everything */
      //统一处理信号（针对触发的，调用其对应的回调）
      quagga_sigevent_process ();
       
      /* Drain the ready queue of already scheduled jobs, before scheduling
       * more.
       */
      //如果ready链上有元素，则自ready链上摘取一个
      if ((thread = thread_trim_head (&m->ready)) != NULL)
        return thread;
      
      /* To be fair to all kinds of threads, and avoid starvation, we
       * need to be careful to consider all thread types for scheduling
       * in each quanta. I.e. we should not return early from here on.
       */
       
      /* Normal event are the next highest priority.  */
      //将m->event链上的元素移至master->ready链
      thread_process (&m->event);
      
      /* Structure copy.  */
      readfd = fd_copy_fd_set(m->readfd);
      writefd = fd_copy_fd_set(m->writefd);
      exceptfd = fd_copy_fd_set(m->exceptfd);
      
      /* Calculate select wait timer if nothing else to do */
      if (m->ready.count == 0)
        {
    	  //获取select应等待的时间（如果有timer,则等待时间应为timer的最近触发时间）
          quagga_get_relative (NULL);
          timer_wait = thread_timer_wait (m->timer, &timer_val);
          timer_wait_bg = thread_timer_wait (m->background, &timer_val_bg);
          
          if (timer_wait_bg &&
              (!timer_wait || (timeval_cmp (*timer_wait, *timer_wait_bg) > 0)))
            timer_wait = timer_wait_bg;
        }
      
      //将read,write,except对应的fd集合放入select等待触发
      num = fd_select (FD_SETSIZE, &readfd, &writefd, &exceptfd, timer_wait);
      
      /* Signals should get quick treatment */
      if (num < 0)
        {
          if (errno == EINTR)
            continue; /* signal received - process it */
          zlog_warn ("select() error: %s", safe_strerror (errno));
          return NULL;
        }

      /* Check foreground timers.  Historically, they have had higher
         priority than I/O threads, so let's push them onto the ready
	 list in front of the I/O threads. */
      //更新时间relative_time
      quagga_get_relative (NULL);

      //定时器处理，用当前时间检查已过期的timer,将其存放在ready链上
      thread_timer_process (m->timer, &relative_time);
      
      /* Got IO, process it */
      //将可read,write的thread添加至master->ready链上
      if (num > 0)
        thread_process_fds (m, &readfd, &writefd, num);

#if 0
      /* If any threads were made ready above (I/O or foreground timer),
         perhaps we should avoid adding background timers to the ready
	 list at this time.  If this is code is uncommented, then background
	 timer threads will not run unless there is nothing else to do. */
      if ((thread = thread_trim_head (&m->ready)) != NULL)
        return thread;
#endif

      /* Background timer/events, lowest priority */
      //处理低优先级的timer
      thread_timer_process (m->background, &relative_time);
      
      if ((thread = thread_trim_head (&m->ready)) != NULL)
        return thread;
    }
}

unsigned long
thread_consumed_time (RUSAGE_T *now, RUSAGE_T *start, unsigned long *cputime)
{
#ifdef HAVE_RUSAGE
  /* This is 'user + sys' time.  */
  *cputime = timeval_elapsed (now->cpu.ru_utime, start->cpu.ru_utime) +
	     timeval_elapsed (now->cpu.ru_stime, start->cpu.ru_stime);
#else
  *cputime = 0;
#endif /* HAVE_RUSAGE */
  return timeval_elapsed (now->real, start->real);
}

/* We should aim to yield after THREAD_YIELD_TIME_SLOT milliseconds. 
   Note: we are using real (wall clock) time for this calculation.
   It could be argued that CPU time may make more sense in certain
   contexts.  The things to consider are whether the thread may have
   blocked (in which case wall time increases, but CPU time does not),
   or whether the system is heavily loaded with other processes competing
   for CPU time.  On balance, wall clock time seems to make sense. 
   Plus it has the added benefit that gettimeofday should be faster
   than calling getrusage. */
int
thread_should_yield (struct thread *thread)
{
  quagga_get_relative (NULL);
  unsigned long t = timeval_elapsed(relative_time, thread->real);
  return ((t > THREAD_YIELD_TIME_SLOT) ? t : 0);
}

//设置real时间
void
thread_getrusage (RUSAGE_T *r)
{
  quagga_get_relative (NULL);
#ifdef HAVE_RUSAGE
  getrusage(RUSAGE_SELF, &(r->cpu));
#endif
  r->real = relative_time;

#ifdef HAVE_CLOCK_MONOTONIC
  /* quagga_get_relative() only updates recent_time if gettimeofday
   * based, not when using CLOCK_MONOTONIC. As we export recent_time
   * and guarantee to update it before threads are run...
   */
  quagga_gettimeofday(&recent_time);
#endif /* HAVE_CLOCK_MONOTONIC */
}

struct thread *thread_current = NULL;

/* We check thread consumed time. If the system has getrusage, we'll
   use that to get in-depth stats on the performance of the thread in addition
   to wall clock time stats from gettimeofday. 
 
   'Dummy' threads (e.g.  see funcname_thread_execute) must have
   thread->master == NULL.
 */
   
static void
thread_call (struct thread *thread)
{
  unsigned long realtime, cputime;
  RUSAGE_T before, after;
 
 /* Cache a pointer to the relevant cpu history thread, if the thread
  * does not have it yet.
  *
  * Callers submitting 'dummy threads' hence must take care that
  * thread->cpu is NULL
  */
  if (!thread->hist)
    {
	  //未记录在cpu_record中，将其记入
      struct cpu_thread_history tmp;
      
      tmp.func = thread->func;
      tmp.funcname = thread->funcname;
      
      thread->hist = hash_get (cpu_record, &tmp, 
                    (void * (*) (void *))cpu_record_hash_alloc);
    }

  GETRUSAGE (&before);
  thread->real = before.real;

  //设置当前正在触发的thread
  thread_current = thread;
  //触发thread
  (*thread->func) (thread);
  thread_current = NULL;

  GETRUSAGE (&after);

  realtime = thread_consumed_time (&after, &before, &cputime);
  //记录此thread回调使用的总体时间
  thread->hist->real.total += realtime;
  if (thread->hist->real.max < realtime)
    thread->hist->real.max = realtime;
#ifdef HAVE_RUSAGE
  thread->hist->cpu.total += cputime;
  if (thread->hist->cpu.max < cputime)
    thread->hist->cpu.max = cputime;
#endif

  ++(thread->hist->total_calls);
  thread->hist->types |= (1 << thread->add_type);

#ifdef CONSUMED_TIME_CHECK
  if (realtime > CONSUMED_TIME_CHECK)
    {
      /*
       * We have a CPU Hog on our hands.
       * Whinge about it now, so we're aware this is yet another task
       * to fix.
       */
      zlog_warn ("SLOW THREAD: task %s (%lx) ran for %lums (cpu time %lums)",
		 thread->funcname,
		 (unsigned long) thread->func,
		 realtime/1000, cputime/1000);
    }
#endif /* CONSUMED_TIME_CHECK */

  //归还thread对象
  thread_add_unuse (thread);
}

/* Execute thread */
struct thread *
funcname_thread_execute (struct thread_master *m,
                int (*func)(struct thread *), 
                void *arg,
                int val,
		debugargdef)
{
  struct thread dummy; 

  memset (&dummy, 0, sizeof (struct thread));

  dummy.type = THREAD_EVENT;
  dummy.add_type = THREAD_EXECUTE;
  dummy.master = NULL;
  dummy.func = func;
  dummy.arg = arg;
  dummy.u.val = val;

  dummy.funcname = funcname;
  dummy.schedfrom = schedfrom;
  dummy.schedfrom_line = fromln;

  thread_call (&dummy);

  return NULL;
}

/* Co-operative thread main loop */
//事件模式处理入口
void
thread_main (struct thread_master *master)
{
  struct thread *t;
  //自master中获得一个event,处理这个event
  while ((t = thread_fetch (master)))
    thread_call (t);
}
