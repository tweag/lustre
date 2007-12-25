/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001, 2002 Cluster File Systems, Inc.
 *   Author: Isaac Huang <isaac@clusterfs.com>
 *
 */

#define DEBUG_SUBSYSTEM S_LNET

#include "selftest.h"


/*
 * Timers are implemented as a sorted queue of expiry times. The queue 
 * is slotted, with each slot holding timers which expire in a 
 * 2**STTIMER_MINPOLL (8) second period. The timers in each slot are 
 * sorted by increasing expiry time. The number of slots is 2**7 (128),
 * to cover a time period of 1024 seconds into the future before wrapping.
 */
#define	STTIMER_MINPOLL        3   /* log2 min poll interval (8 s) */
#define	STTIMER_SLOTTIME       (1 << STTIMER_MINPOLL)
#define	STTIMER_SLOTTIMEMASK   (~(STTIMER_SLOTTIME - 1))
#define	STTIMER_NSLOTS	       (1 << 7)
#define	STTIMER_SLOT(t)	       (&stt_data.stt_hash[(((t) >> STTIMER_MINPOLL) & \
                                                    (STTIMER_NSLOTS - 1))])

struct st_timer_data {
        spinlock_t       stt_lock;
        /* start time of the slot processed previously */
        cfs_time_t       stt_prev_slot; 
        struct list_head stt_hash[STTIMER_NSLOTS];
        int              stt_shuttingdown;
#ifdef __KERNEL__
        cfs_waitq_t      stt_waitq;
        int              stt_nthreads;
#endif
} stt_data;

void
stt_add_timer (stt_timer_t *timer)
{
        struct list_head *pos;

        spin_lock(&stt_data.stt_lock);

#ifdef __KERNEL__
        LASSERT (stt_data.stt_nthreads > 0);
#endif
        LASSERT (!stt_data.stt_shuttingdown);
        LASSERT (timer->stt_func != NULL);
        LASSERT (list_empty(&timer->stt_list));
        LASSERT (cfs_time_after(timer->stt_expires, cfs_time_current_sec()));

        /* a simple insertion sort */
        list_for_each_prev (pos, STTIMER_SLOT(timer->stt_expires)) {
                stt_timer_t *old = list_entry(pos, stt_timer_t, stt_list);

                if (cfs_time_aftereq(timer->stt_expires, old->stt_expires))
                        break;
        }
        list_add(&timer->stt_list, pos);

        spin_unlock(&stt_data.stt_lock);
}

/*
 * The function returns whether it has deactivated a pending timer or not.
 * (ie. del_timer() of an inactive timer returns 0, del_timer() of an
 * active timer returns 1.)
 *
 * CAVEAT EMPTOR:
 * When 0 is returned, it is possible that timer->stt_func _is_ running on
 * another CPU.
 */
int
stt_del_timer (stt_timer_t *timer)
{
        int ret = 0;

        spin_lock(&stt_data.stt_lock);

#ifdef __KERNEL__
        LASSERT (stt_data.stt_nthreads > 0);
#endif
        LASSERT (!stt_data.stt_shuttingdown);

        if (!list_empty(&timer->stt_list)) {
                ret = 1;
                list_del_init(&timer->stt_list);
        }

        spin_unlock(&stt_data.stt_lock);
        return ret;
}

/* called with stt_data.stt_lock held */
int
stt_expire_list (struct list_head *slot, cfs_time_t now)
{
        int          expired = 0;
        stt_timer_t *timer;

        while (!list_empty(slot)) {
                timer = list_entry(slot->next, stt_timer_t, stt_list);

                if (cfs_time_after(timer->stt_expires, now))
                        break;

                list_del_init(&timer->stt_list);
                spin_unlock(&stt_data.stt_lock);

                expired++;
                (*timer->stt_func) (timer->stt_data);
                
                spin_lock(&stt_data.stt_lock);
        }

        return expired;
}

int
stt_check_timers (cfs_time_t *last)
{
        int        expired = 0;
        cfs_time_t now;
        cfs_time_t this_slot;

        now = cfs_time_current_sec();
        this_slot = now & STTIMER_SLOTTIMEMASK;

        spin_lock(&stt_data.stt_lock);

        while (cfs_time_aftereq(this_slot, *last)) {
                expired += stt_expire_list(STTIMER_SLOT(this_slot), now);
                this_slot = cfs_time_sub(this_slot, STTIMER_SLOTTIME);
        }

        *last = now & STTIMER_SLOTTIMEMASK;
        spin_unlock(&stt_data.stt_lock);
        return expired;
}

#ifdef __KERNEL__

int
stt_timer_main (void *arg)
{
        UNUSED(arg);

        cfs_daemonize("st_timer");
        cfs_block_allsigs();

        while (!stt_data.stt_shuttingdown) {
                stt_check_timers(&stt_data.stt_prev_slot);

                cfs_waitq_wait_event_timeout(stt_data.stt_waitq,
                                   stt_data.stt_shuttingdown,
                                   cfs_time_seconds(STTIMER_SLOTTIME));
        }

        spin_lock(&stt_data.stt_lock);
        stt_data.stt_nthreads--;
        spin_unlock(&stt_data.stt_lock);
        return 0;
}

int
stt_start_timer_thread (void)
{
        long pid;

        LASSERT (!stt_data.stt_shuttingdown);

        pid = cfs_kernel_thread(stt_timer_main, NULL, 0);
        if (pid < 0)
                return (int)pid;

        spin_lock(&stt_data.stt_lock);
        stt_data.stt_nthreads++;
        spin_unlock(&stt_data.stt_lock);
        return 0;
}

#else /* !__KERNEL__ */

int
stt_check_events (void)
{
        return stt_check_timers(&stt_data.stt_prev_slot);
}

int
stt_poll_interval (void)
{
        return STTIMER_SLOTTIME;
}

#endif

int
stt_startup (void)
{
        int rc = 0;
        int i;

        stt_data.stt_shuttingdown = 0;
        stt_data.stt_prev_slot = cfs_time_current_sec() & STTIMER_SLOTTIMEMASK;

        spin_lock_init(&stt_data.stt_lock);
        for (i = 0; i < STTIMER_NSLOTS; i++)
                CFS_INIT_LIST_HEAD(&stt_data.stt_hash[i]);

#ifdef __KERNEL__
        stt_data.stt_nthreads = 0;
        cfs_waitq_init(&stt_data.stt_waitq);
        rc = stt_start_timer_thread();
        if (rc != 0)
                CERROR ("Can't spawn timer thread: %d\n", rc);
#endif

        return rc;
}

void
stt_shutdown (void)
{
        int i;

        spin_lock(&stt_data.stt_lock);

        for (i = 0; i < STTIMER_NSLOTS; i++)
                LASSERT (list_empty(&stt_data.stt_hash[i]));

        stt_data.stt_shuttingdown = 1;

#ifdef __KERNEL__
        cfs_waitq_signal(&stt_data.stt_waitq);
        lst_wait_until(stt_data.stt_nthreads == 0, stt_data.stt_lock,
                       "waiting for %d threads to terminate\n",
                       stt_data.stt_nthreads);
#endif

        spin_unlock(&stt_data.stt_lock);
        return;
}
