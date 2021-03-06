/*
 * kernel/mutex.c
 *
 * Mutexes: blocking mutual exclusion locks
 *
 * Started by Ingo Molnar:
 *
 *  Copyright (C) 2004, 2005, 2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 * Many thanks to Arjan van de Ven, Thomas Gleixner, Steven Rostedt and
 * David Howells for suggestions and improvements.
 *
 *  - Adaptive spinning for mutexes by Peter Zijlstra. (Ported to mainline
 *    from the -rt tree, where it was originally implemented for rtmutexes
 *    by Steven Rostedt, based on work by Gregory Haskins, Peter Morreale
 *    and Sven Dietrich.
 *
 * Also see Documentation/mutex-design.txt.
 */
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/debug_locks.h>

#ifdef CONFIG_DEBUG_MUTEXES
# include "mutex-debug.h"
# include <asm-generic/mutex-null.h>
#else
# include "mutex.h"
# include <asm/mutex.h>
#endif

/*
 * A mutex count of -1 indicates that waiters are sleeping waiting for the
 * mutex. Some architectures can allow any negative number, not just -1, for
 * this purpose.
 */
#ifdef __ARCH_ALLOW_ANY_NEGATIVE_MUTEX_COUNT
#define	MUTEX_SHOW_NO_WAITER(mutex)	(atomic_read(&(mutex)->count) >= 0)
#else
#define	MUTEX_SHOW_NO_WAITER(mutex)	(atomic_read(&(mutex)->count) != -1)
#endif

void
__mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key)
{
	atomic_set(&lock->count, 1);
	spin_lock_init(&lock->wait_lock);
	INIT_LIST_HEAD(&lock->wait_list);
	mutex_clear_owner(lock);
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	lock->spin_mlock = NULL;
#endif

	debug_mutex_init(lock, name, key);
}

EXPORT_SYMBOL(__mutex_init);

#ifndef CONFIG_DEBUG_LOCK_ALLOC
static __used noinline void __sched
__mutex_lock_slowpath(atomic_t *lock_count);

void __sched mutex_lock(struct mutex *lock)
{
	might_sleep();
	__mutex_fastpath_lock(&lock->count, __mutex_lock_slowpath);
	mutex_set_owner(lock);
}

EXPORT_SYMBOL(mutex_lock);
#endif

#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
/*
 * In order to avoid a stampede of mutex spinners from acquiring the mutex
 * more or less simultaneously, the spinners need to acquire a MCS lock
 * first before spinning on the owner field.
 *
 * We don't inline mspin_lock() so that perf can correctly account for the
 * time spent in this lock function.
 */
typedef struct mspin_node {
	struct mspin_node *next;
	int		   locked;	/* 1 if lock acquired */
} mspin_node_t;

typedef mspin_node_t	*mspin_lock_t;

#define	MLOCK(mutex)	((mspin_lock_t *)&((mutex)->spin_mlock))

static noinline void mspin_lock(mspin_lock_t *lock,  mspin_node_t *node)
{
	mspin_node_t *prev;

	/* Init node */
	node->locked = 0;
	node->next   = NULL;

	prev = xchg(lock, node);
	if (likely(prev == NULL)) {
		/* Lock acquired */
		node->locked = 1;
		return;
	}
	ACCESS_ONCE(prev->next) = node;
	smp_wmb();
	/* Wait until the lock holder passes the lock down */
	while (!ACCESS_ONCE(node->locked))
		arch_mutex_cpu_relax();
}

static void mspin_unlock(mspin_lock_t *lock,  mspin_node_t *node)
{
	mspin_node_t *next = ACCESS_ONCE(node->next);

	if (likely(!next)) {
		/*
		 * Release the lock by setting it to NULL
		 */
		if (cmpxchg(lock, node, NULL) == node)
			return;
		/* Wait until the next pointer is set */
		while (!(next = ACCESS_ONCE(node->next)))
			arch_mutex_cpu_relax();
	}
	barrier();
	ACCESS_ONCE(next->locked) = 1;
	smp_wmb();
}
#endif

static __used noinline void __sched __mutex_unlock_slowpath(atomic_t *lock_count);

void __sched mutex_unlock(struct mutex *lock)
{
#ifndef CONFIG_DEBUG_MUTEXES
	mutex_clear_owner(lock);
#endif
	__mutex_fastpath_unlock(&lock->count, __mutex_unlock_slowpath);
}

EXPORT_SYMBOL(mutex_unlock);

static inline int __sched
__mutex_lock_common(struct mutex *lock, long state, unsigned int subclass,
		    struct lockdep_map *nest_lock, unsigned long ip)
{
	struct task_struct *task = current;
	struct mutex_waiter waiter;
	unsigned long flags;

	preempt_disable();
	mutex_acquire_nest(&lock->dep_map, subclass, 0, nest_lock, ip);

#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	/*
	 * Optimistic spinning.
	 *
	 * We try to spin for acquisition when we find that there are no
	 * pending waiters and the lock owner is currently running on a
	 * (different) CPU.
	 *
	 * The rationale is that if the lock owner is running, it is likely to
	 * release the lock soon.
	 *
	 * Since this needs the lock owner, and this mutex implementation
	 * doesn't track the owner atomically in the lock field, we need to
	 * track it non-atomically.
	 *
	 * We can't do this for DEBUG_MUTEXES because that relies on wait_lock
	 * to serialize everything.
	 *
	 * The mutex spinners are queued up using MCS lock so that only one
	 * spinner can compete for the mutex. However, if mutex spinning isn't
	 * going to happen, there is no point in going through the lock/unlock
	 * overhead.
	 */
	if (!mutex_can_spin_on_owner(lock))
		goto slowpath;

	for (;;) {
		struct task_struct *owner;
		mspin_node_t	    node;

		/*
		 * If there's an owner, wait for it to either
		 * release the lock or go to sleep.
		 */
		mspin_lock(MLOCK(lock), &node);
		owner = ACCESS_ONCE(lock->owner);
		if (owner && !mutex_spin_on_owner(lock, owner)) {
			mspin_unlock(MLOCK(lock), &node);
			break;
		}

		if ((atomic_read(&lock->count) == 1) &&
		    (atomic_cmpxchg(&lock->count, 1, 0) == 1)) {
			lock_acquired(&lock->dep_map, ip);
			mutex_set_owner(lock);
			mspin_unlock(MLOCK(lock), &node);
			preempt_enable();
			return 0;
		}
		mspin_unlock(MLOCK(lock), &node);

		if (!owner && (need_resched() || rt_task(task)))
			break;

		arch_mutex_cpu_relax();
	}
slowpath:
#endif
	spin_lock_mutex(&lock->wait_lock, flags);

	debug_mutex_lock_common(lock, &waiter);
	debug_mutex_add_waiter(lock, &waiter, task_thread_info(task));

	
	list_add_tail(&waiter.list, &lock->wait_list);
	waiter.task = task;

	if (MUTEX_SHOW_NO_WAITER(lock) && (atomic_xchg(&lock->count, -1) == 1))
		goto done;

	lock_contended(&lock->dep_map, ip);

	for (;;) {
		/*
		 * Lets try to take the lock again - this is needed even if
		 * we get here for the first time (shortly after failing to
		 * acquire the lock), to make sure that we get a wakeup once
		 * it's unlocked. Later on, if we sleep, this is the
		 * operation that gives us the lock. We xchg it to -1, so
		 * that when we release the lock, we properly wake up the
		 * other waiters:
		 */
		if (MUTEX_SHOW_NO_WAITER(lock) &&
		   (atomic_xchg(&lock->count, -1) == 1))
			break;

		if (unlikely(signal_pending_state(state, task))) {
			mutex_remove_waiter(lock, &waiter,
					    task_thread_info(task));
			mutex_release(&lock->dep_map, 1, ip);
			spin_unlock_mutex(&lock->wait_lock, flags);

			debug_mutex_free_waiter(&waiter);
			preempt_enable();
			return -EINTR;
		}
		__set_task_state(task, state);

		
		spin_unlock_mutex(&lock->wait_lock, flags);
		schedule_preempt_disabled();
		spin_lock_mutex(&lock->wait_lock, flags);
	}

done:
	lock_acquired(&lock->dep_map, ip);
	
	mutex_remove_waiter(lock, &waiter, current_thread_info());
	mutex_set_owner(lock);

	
	if (likely(list_empty(&lock->wait_list)))
		atomic_set(&lock->count, 0);

	spin_unlock_mutex(&lock->wait_lock, flags);

	debug_mutex_free_waiter(&waiter);
	preempt_enable();

	return 0;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
void __sched
mutex_lock_nested(struct mutex *lock, unsigned int subclass)
{
	might_sleep();
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, subclass, NULL, _RET_IP_);
}

EXPORT_SYMBOL_GPL(mutex_lock_nested);

void __sched
_mutex_lock_nest_lock(struct mutex *lock, struct lockdep_map *nest)
{
	might_sleep();
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, 0, nest, _RET_IP_);
}

EXPORT_SYMBOL_GPL(_mutex_lock_nest_lock);

int __sched
mutex_lock_killable_nested(struct mutex *lock, unsigned int subclass)
{
	might_sleep();
	return __mutex_lock_common(lock, TASK_KILLABLE, subclass, NULL, _RET_IP_);
}
EXPORT_SYMBOL_GPL(mutex_lock_killable_nested);

int __sched
mutex_lock_interruptible_nested(struct mutex *lock, unsigned int subclass)
{
	might_sleep();
	return __mutex_lock_common(lock, TASK_INTERRUPTIBLE,
				   subclass, NULL, _RET_IP_);
}

EXPORT_SYMBOL_GPL(mutex_lock_interruptible_nested);
#endif

static inline void
__mutex_unlock_common_slowpath(atomic_t *lock_count, int nested)
{
	struct mutex *lock = container_of(lock_count, struct mutex, count);
	unsigned long flags;

	spin_lock_mutex(&lock->wait_lock, flags);
	mutex_release(&lock->dep_map, nested, _RET_IP_);
	debug_mutex_unlock(lock);

	if (__mutex_slowpath_needs_to_unlock())
		atomic_set(&lock->count, 1);

	if (!list_empty(&lock->wait_list)) {
		
		struct mutex_waiter *waiter =
				list_entry(lock->wait_list.next,
					   struct mutex_waiter, list);

		debug_mutex_wake_waiter(lock, waiter);

		wake_up_process(waiter->task);
	}

	spin_unlock_mutex(&lock->wait_lock, flags);
}

static __used noinline void
__mutex_unlock_slowpath(atomic_t *lock_count)
{
	__mutex_unlock_common_slowpath(lock_count, 1);
}

#ifndef CONFIG_DEBUG_LOCK_ALLOC
static noinline int __sched
__mutex_lock_killable_slowpath(atomic_t *lock_count);

static noinline int __sched
__mutex_lock_interruptible_slowpath(atomic_t *lock_count);

int __sched mutex_lock_interruptible(struct mutex *lock)
{
	int ret;

	might_sleep();
	ret =  __mutex_fastpath_lock_retval
			(&lock->count, __mutex_lock_interruptible_slowpath);
	if (!ret)
		mutex_set_owner(lock);

	return ret;
}

EXPORT_SYMBOL(mutex_lock_interruptible);

int __sched mutex_lock_killable(struct mutex *lock)
{
	int ret;

	might_sleep();
	ret = __mutex_fastpath_lock_retval
			(&lock->count, __mutex_lock_killable_slowpath);
	if (!ret)
		mutex_set_owner(lock);

	return ret;
}
EXPORT_SYMBOL(mutex_lock_killable);

static __used noinline void __sched
__mutex_lock_slowpath(atomic_t *lock_count)
{
	struct mutex *lock = container_of(lock_count, struct mutex, count);

	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, 0, NULL, _RET_IP_);
}

static noinline int __sched
__mutex_lock_killable_slowpath(atomic_t *lock_count)
{
	struct mutex *lock = container_of(lock_count, struct mutex, count);

	return __mutex_lock_common(lock, TASK_KILLABLE, 0, NULL, _RET_IP_);
}

static noinline int __sched
__mutex_lock_interruptible_slowpath(atomic_t *lock_count)
{
	struct mutex *lock = container_of(lock_count, struct mutex, count);

	return __mutex_lock_common(lock, TASK_INTERRUPTIBLE, 0, NULL, _RET_IP_);
}
#endif

static inline int __mutex_trylock_slowpath(atomic_t *lock_count)
{
	struct mutex *lock = container_of(lock_count, struct mutex, count);
	unsigned long flags;
	int prev;

	spin_lock_mutex(&lock->wait_lock, flags);

	prev = atomic_xchg(&lock->count, -1);
	if (likely(prev == 1)) {
		mutex_set_owner(lock);
		mutex_acquire(&lock->dep_map, 0, 1, _RET_IP_);
	}

	
	if (likely(list_empty(&lock->wait_list)))
		atomic_set(&lock->count, 0);

	spin_unlock_mutex(&lock->wait_lock, flags);

	return prev == 1;
}

int __sched mutex_trylock(struct mutex *lock)
{
	int ret;

	ret = __mutex_fastpath_trylock(&lock->count, __mutex_trylock_slowpath);
	if (ret)
		mutex_set_owner(lock);

	return ret;
}
EXPORT_SYMBOL(mutex_trylock);

int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	
	if (atomic_add_unless(cnt, -1, 1))
		return 0;
	
	mutex_lock(lock);
	if (!atomic_dec_and_test(cnt)) {
		
		mutex_unlock(lock);
		return 0;
	}
	
	return 1;
}
EXPORT_SYMBOL(atomic_dec_and_mutex_lock);
