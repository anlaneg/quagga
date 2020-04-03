/* Thread management routine header.
 * Copyright (C) 1998 Kunihiro Ishiguro
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

#ifndef _ZEBRA_THREAD_H
#define _ZEBRA_THREAD_H

#include <zebra.h>

struct rusage_t
{
#ifdef HAVE_RUSAGE
  struct rusage cpu;
#endif
  struct timeval real;
};
#define RUSAGE_T        struct rusage_t

#define GETRUSAGE(X) thread_getrusage(X)

/* Linked list of thread. */
struct thread_list
{
  struct thread *head;
  struct thread *tail;
  int count;
};

struct pqueue;

/*
 * Abstract it so we can use different methodologies to
 * select on data.
 */
typedef fd_set thread_fd_set;

/* Master of the theads. */
struct thread_master
{
  //记录需要read的thread
  struct thread **read;
  //记录需要write的thread
  struct thread **write;
  struct pqueue *timer;
  struct thread_list event;//event链表（高优先级）
  struct thread_list ready;
  struct thread_list unuse;/*用于串连没有使用的struct thread*/
  struct pqueue *background;//记录低优先级的timer及event
  int fd_limit;
  //记录需要read的fd
  thread_fd_set readfd;
  //记录需要write的fd
  thread_fd_set writefd;
  thread_fd_set exceptfd;
  unsigned long alloc;/*申请的thread总数*/
};

typedef unsigned char thread_type;

/* Thread itself. */
struct thread
{
   /*thread类型，例如THREAD_READ*/
  thread_type type;		/* thread type */
  thread_type add_type;		/* thread type */
  struct thread *next;		/* next pointer of the thread */   
  struct thread *prev;		/* previous pointer of the thread */
  //从属的thread_master
  struct thread_master *master;	/* pointer to the struct thread_master. */
  /*event处理函数*/
  int (*func) (struct thread *); /* event function */
  /*event参数*/
  void *arg;			/* event argument */
  /*附加的event参数*/
  union {
    int val;			/* second argument of the event. */
    int fd;			/* file descriptor in case of read/write. */
    struct timeval sands;	/* rest of time sands value. */
  } u;
  int index;			/* used for timers to store position in queue */
  struct timeval real;
  struct cpu_thread_history *hist; /* cache pointer to cpu_history */
  //thread创建时的函数名，文件名及行号
  const char *funcname;
  const char *schedfrom;
  int schedfrom_line;
};

struct cpu_thread_history 
{
  int (*func)(struct thread *);
  unsigned int total_calls;//总调用次数
  struct time_stats
  {
    unsigned long total/*总时间*/, max/*最大单次时间*/;
  } real;
#ifdef HAVE_RUSAGE
  struct time_stats cpu;
#endif
  thread_type types;
  const char *funcname;
};

/* Clocks supported by Quagga */
enum quagga_clkid {
  QUAGGA_CLK_REALTIME = 0,	/* ala gettimeofday() */
  QUAGGA_CLK_MONOTONIC,		/* monotonic, against an indeterminate base */
  QUAGGA_CLK_REALTIME_STABILISED, /* like realtime, but non-decrementing */
};

/* Thread types. */
//现支持的所有事件类型
#define THREAD_READ           0
#define THREAD_WRITE          1
#define THREAD_TIMER          2
#define THREAD_EVENT          3
#define THREAD_READY          4
#define THREAD_BACKGROUND     5
#define THREAD_UNUSED         6
#define THREAD_EXECUTE        7

/* Thread yield time.  */
#define THREAD_YIELD_TIME_SLOT     10 * 1000L /* 10ms */

/* Macros. */
#define THREAD_ARG(X) ((X)->arg)
#define THREAD_FD(X)  ((X)->u.fd)
#define THREAD_VAL(X) ((X)->u.val)

//如果thread为NULL，则添加read thread
#define THREAD_READ_ON(master,thread,func,arg,sock) \
  do { \
    if (! thread) \
      thread = thread_add_read (master, func, arg, sock); \
  } while (0)

#define THREAD_WRITE_ON(master,thread,func,arg,sock) \
  do { \
    if (! thread) \
      thread = thread_add_write (master, func, arg, sock); \
  } while (0)

#define THREAD_TIMER_ON(master,thread,func,arg,time) \
  do { \
    if (! thread) \
      thread = thread_add_timer (master, func, arg, time); \
  } while (0)

#define THREAD_TIMER_MSEC_ON(master,thread,func,arg,time) \
  do { \
    if (! thread) \
      thread = thread_add_timer_msec (master, func, arg, time); \
  } while (0)

#define THREAD_OFF(thread) \
  do { \
    if (thread) \
      { \
        thread_cancel (thread); \
        thread = NULL; \
      } \
  } while (0)

#define THREAD_READ_OFF(thread)  THREAD_OFF(thread)
#define THREAD_WRITE_OFF(thread)  THREAD_OFF(thread)
#define THREAD_TIMER_OFF(thread)  THREAD_OFF(thread)

#define debugargdef  const char *funcname/*函数名称*/, const char *schedfrom/*文件名称*/, int fromln/*行号*/

//新建read 任务
#define thread_add_read(m,f,a,v) funcname_thread_add_read(m,f,a,v,#f,__FILE__,__LINE__)
//新建write任务
#define thread_add_write(m,f,a,v) funcname_thread_add_write(m,f,a,v,#f,__FILE__,__LINE__)
//新建timer任务
#define thread_add_timer(m,f,a,v) funcname_thread_add_timer(m,f,a,v,#f,__FILE__,__LINE__)
#define thread_add_timer_msec(m,f,a,v) funcname_thread_add_timer_msec(m,f,a,v,#f,__FILE__,__LINE__)
#define thread_add_timer_tv(m,f,a,v) funcname_thread_add_timer_tv(m,f,a,v,#f,__FILE__,__LINE__)
//新建event事件
#define thread_add_event(m,f,a,v) funcname_thread_add_event(m,f,a,v,#f,__FILE__,__LINE__)
#define thread_execute(m,f,a,v) funcname_thread_execute(m,f,a,v,#f,__FILE__,__LINE__)

/* The 4th arg to thread_add_background is the # of milliseconds to delay. */
#define thread_add_background(m,f,a,v) funcname_thread_add_background(m,f,a,v,#f,__FILE__,__LINE__)

/* Prototypes. */
extern struct thread_master *thread_master_create (void);
extern void thread_master_free (struct thread_master *);

extern struct thread *funcname_thread_add_read (struct thread_master *, 
				                int (*)(struct thread *),
				                void *, int, debugargdef);
extern struct thread *funcname_thread_add_write (struct thread_master *,
				                 int (*)(struct thread *),
				                 void *, int, debugargdef);
extern struct thread *funcname_thread_add_timer (struct thread_master *,
				                 int (*)(struct thread *),
				                 void *, long, debugargdef);
extern struct thread *funcname_thread_add_timer_msec (struct thread_master *,
				                      int (*)(struct thread *),
				                      void *, long, debugargdef);
extern struct thread *funcname_thread_add_timer_tv (struct thread_master *,
				                    int (*)(struct thread *),
				                    void *, struct timeval *,
						    debugargdef);
extern struct thread *funcname_thread_add_event (struct thread_master *,
				                 int (*)(struct thread *),
				                 void *, int, debugargdef);
extern struct thread *funcname_thread_add_background (struct thread_master *,
                                               int (*func)(struct thread *),
				               void *arg,
				               long milliseconds_to_delay,
					       debugargdef);
extern struct thread *funcname_thread_execute (struct thread_master *,
                                               int (*)(struct thread *),
                                               void *, int, debugargdef);
#undef debugargdef

extern void thread_cancel (struct thread *);
extern unsigned int thread_cancel_event (struct thread_master *, void *);
extern void thread_main (struct thread_master *);
extern unsigned long thread_timer_remain_second (struct thread *);
extern struct timeval thread_timer_remain(struct thread*);
extern int thread_should_yield (struct thread *);
extern unsigned long timeval_elapsed (struct timeval a, struct timeval b);

/* Internal libzebra exports */
extern void thread_getrusage (RUSAGE_T *);
extern struct cmd_element show_thread_cpu_cmd;
extern struct cmd_element clear_thread_cpu_cmd;

/* replacements for the system gettimeofday(), clock_gettime() and
 * time() functions, providing support for non-decrementing clock on
 * all systems, and fully monotonic on /some/ systems.
 */
extern int quagga_gettime (enum quagga_clkid, struct timeval *);
extern time_t quagga_time (time_t *);

/* Returns elapsed real (wall clock) time. */
extern unsigned long thread_consumed_time(RUSAGE_T *after, RUSAGE_T *before,
					  unsigned long *cpu_time_elapsed);

/* Global variable containing a recent result from gettimeofday.  This can
   be used instead of calling gettimeofday if a recent value is sufficient.
   This is guaranteed to be refreshed before a thread is called. */
extern struct timeval recent_time;
/* Similar to recent_time, but a monotonically increasing time value */
extern struct timeval recent_relative_time (void);

/* only for use in logging functions! */
extern struct thread *thread_current;

#endif /* _ZEBRA_THREAD_H */
