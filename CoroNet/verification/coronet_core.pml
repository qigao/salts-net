/*
 * CoroNet core lifecycle model.
 *
 * Scope:
 *   - bounded MPSC post queue with one event-loop consumer
 *   - callback re-entry after the consumed slot is released
 *   - timer versus cancellation completion race for one wait
 *   - stop-after-producers shutdown and final context ownership checks
 *
 * The model deliberately uses a small queue.  It preserves full/empty and
 * re-entry transitions without making the state space depend on the runtime
 * queue capacity.
 */

mtype = { POST_CB, TIMER_CB, CANCEL_CB };

#define POST_CAP 2
#define POST_PRODUCERS 2
#define POSTS_PER_PRODUCER 2
#define WAIT_ACTIVE 1
#define WAIT_IDLE 0

chan post_queue = [POST_CAP] of { mtype };

byte post_count = 0;
byte post_accepted = 0;
byte post_delivered = 0;
byte wake_pending = 0;

byte wait_state = WAIT_ACTIVE;
byte wait_registration = 1;
byte wait_resume_count = 0;
byte wait_result = 0;
byte external_refs = 1;

byte cancel_requested = 0;
byte cancel_dispatch_pending = 0;
byte cancel_callback_count = 0;
byte source_destroyed = 0;

byte timer_posted = 0;
byte cancel_posted = 0;
byte producer_done = 0;
byte stop_requested = 0;
byte run_exited = 0;
byte context_freed = 0;
byte reentrant_post_pending = 1;
byte reentrant_post_attempted = 0;
byte reentrant_post_accepted = 0;

/* The reservation and publication steps are compressed into one transition.
 * The queue count still models the capacity contract and the consumer release
 * happens before callback re-entry, matching drain_post_queue(). */
inline try_post(kind, accepted) {
    atomic {
        if
        :: len(post_queue) < POST_CAP ->
            post_queue ! kind;
            post_count++;
            post_accepted++;
            wake_pending = 1;
            accepted = 1
        :: else ->
            accepted = 0
        fi
    }
}

active proctype EventLoop() {
    mtype kind;
    byte accepted;

    do
    :: len(post_queue) > 0 ->
        /* Consumer release is atomic with dequeue.  User callback execution
         * remains outside this block so it can interleave with producers. */
        atomic {
            post_queue ? kind;
            assert(post_count > 0);
            post_count--;
            post_delivered++;
            assert(post_count < POST_CAP);

            if
            :: post_count == 0 -> wake_pending = 0
            :: else -> skip
            fi
        };

        progress_post_drain: skip;

        if
        :: kind == POST_CB ->
            /* The consumed slot is available before user callback entry. */
            if
            :: reentrant_post_pending == 1 ->
                reentrant_post_pending = 0;
                reentrant_post_attempted = 1;
                try_post(POST_CB, accepted);
                if
                :: accepted == 1 -> reentrant_post_accepted = 1
                :: else -> skip
                fi
            :: else -> skip
            fi

        :: kind == TIMER_CB ->
            if
            :: wait_state == WAIT_ACTIVE ->
                wait_state = WAIT_IDLE;
                wait_result = 1;
                wait_resume_count++
            :: else -> skip
            fi

        :: kind == CANCEL_CB ->
            /* Dispatch is cleared before invoking the registered callback. */
            cancel_dispatch_pending = 0;
            cancel_callback_count++;
            if
            :: wait_state == WAIT_ACTIVE ->
                wait_state = WAIT_IDLE;
                wait_result = 2;
                wait_resume_count++
            :: else -> skip
            fi
        fi;

    :: stop_requested == 1 && post_count == 0 ->
        run_exited = 1;
        break
    od
}

active [POST_PRODUCERS] proctype PostProducer() {
    byte accepted;
    byte sent = 0;

    do
    :: sent < POSTS_PER_PRODUCER ->
        try_post(POST_CB, accepted);
        if
        :: accepted == 1 -> sent++
        :: else -> skip
        fi
    :: else ->
        atomic { producer_done++ };
        break
    od
}

active proctype TimerProducer() {
    byte accepted;

    do
    :: timer_posted == 0 ->
        try_post(TIMER_CB, accepted);
        if
        :: accepted == 1 ->
            timer_posted = 1;
            break
        :: else -> skip
        fi
    od
}

active proctype CancelProducer() {
    byte accepted;

    cancel_requested = 1;
    cancel_dispatch_pending = 1;

    do
    :: cancel_posted == 0 ->
        try_post(CANCEL_CB, accepted);
        if
        :: accepted == 1 ->
            cancel_posted = 1;
            break
        :: else -> skip
        fi
    od
}

active proctype Waiter() {
    do
    :: wait_state == WAIT_ACTIVE -> skip
    :: wait_state == WAIT_IDLE ->
        atomic {
            wait_registration = 0;
            external_refs--;
        };
        break
    od
}

active proctype CancelSourceOwner() {
    do
    :: source_destroyed == 0 &&
       wait_registration == 0 &&
       cancel_dispatch_pending == 0 ->
        source_destroyed = 1;
        break
    :: source_destroyed == 1 -> break
    :: else -> skip
    od
}

active proctype Stopper() {
    do
    :: producer_done == POST_PRODUCERS &&
       timer_posted == 1 &&
       cancel_posted == 1 ->
        stop_requested = 1;
        break
    :: else -> skip
    od
}

active proctype ContextOwner() {
    do
    :: run_exited == 1 &&
       post_count == 0 &&
       external_refs == 0 &&
       wait_state == WAIT_IDLE &&
       wait_registration == 0 &&
       cancel_dispatch_pending == 0 &&
       source_destroyed == 1 ->
        assert(post_accepted == post_delivered);
        context_freed = 1;
        break
    :: else -> skip
    od
}

ltl no_double_resume {
    [] (wait_resume_count <= 1)
}

ltl cancellation_is_single_dispatch {
    [] (cancel_callback_count <= 1)
}

ltl stop_drains_posts {
    [] (run_exited -> post_count == 0)
}

ltl active_wait_eventually_completes {
    [] (wait_state == WAIT_ACTIVE -> <> (wait_state == WAIT_IDLE))
}
