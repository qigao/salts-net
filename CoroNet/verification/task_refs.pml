/*
 * CoroNet lazy-task ownership model.
 *
 * Scope:
 *   - create/start/cancel/destroy state transitions
 *   - context ownership through the initial task reference
 *   - when_all() and when_any() temporary references
 *   - cleanup only after done and ref_count == 0
 *
 * Two started tasks exercise the combinators.  A third task exercises the
 * cancel-before-start path independently.
 */

#define TASK_COUNT 3
#define STARTED_TASK_COUNT 2
#define TASK_A 0
#define TASK_B 1
#define TASK_CANCELLED 2

byte task_started[TASK_COUNT];
byte task_cancelled[TASK_COUNT];
byte task_done[TASK_COUNT];
byte task_destroyed[TASK_COUNT];
byte task_ref_count[TASK_COUNT];

byte all_held = 0;
byte all_returned = 0;
byte any_held = 0;
byte any_returned = 0;
byte any_winner = 255;
byte caller_released = 0;
byte task_complete_count = 0;
byte cancelled_start_attempted = 0;
byte cancelled_start_rejected = 0;

#define both_started (task_started[TASK_A] && task_started[TASK_B])
#define both_done (task_done[TASK_A] && task_done[TASK_B])
#define any_winner_done ((any_winner == TASK_A && task_done[TASK_A]) || \
                         (any_winner == TASK_B && task_done[TASK_B]))
#define all_tasks_destroyed (task_destroyed[TASK_A] && \
                             task_destroyed[TASK_B] && \
                             task_destroyed[TASK_CANCELLED])

proctype TaskRunner(byte id) {
    do
    :: task_started[id] == 0 -> skip
    :: task_started[id] == 1 ->
        atomic {
            assert(task_destroyed[id] == 0);
            task_done[id] = 1;
            task_complete_count++;
        }
        progress_task_done: skip;
        break
    od
}

proctype Cleanup() {
    do
    :: task_done[TASK_A] && task_ref_count[TASK_A] == 0 &&
       task_destroyed[TASK_A] == 0 ->
        task_destroyed[TASK_A] = 1
    :: task_done[TASK_B] && task_ref_count[TASK_B] == 0 &&
       task_destroyed[TASK_B] == 0 ->
        task_destroyed[TASK_B] = 1
    :: task_done[TASK_CANCELLED] && task_ref_count[TASK_CANCELLED] == 0 &&
       task_destroyed[TASK_CANCELLED] == 0 ->
        task_destroyed[TASK_CANCELLED] = 1
    :: all_tasks_destroyed -> break
    :: else -> skip
    od
}

proctype WhenAll() {
    atomic {
        assert(task_destroyed[TASK_A] == 0);
        assert(task_destroyed[TASK_B] == 0);
        task_ref_count[TASK_A]++;
        task_ref_count[TASK_B]++;
        all_held = 1;
    }

    do
    :: both_done -> break
    :: else ->
        assert(task_destroyed[TASK_A] == 0);
        assert(task_destroyed[TASK_B] == 0)
    od;

    atomic {
        task_ref_count[TASK_A]--;
        task_ref_count[TASK_B]--;
        all_returned = 1;
    }
}

proctype WhenAny() {
    atomic {
        assert(task_destroyed[TASK_A] == 0);
        assert(task_destroyed[TASK_B] == 0);
        task_ref_count[TASK_A]++;
        task_ref_count[TASK_B]++;
        any_held = 1;
    }

    do
    :: task_done[TASK_A] ->
        any_winner = TASK_A;
        break
    :: !task_done[TASK_A] && task_done[TASK_B] ->
        any_winner = TASK_B;
        break
    :: else ->
        assert(task_destroyed[TASK_A] == 0);
        assert(task_destroyed[TASK_B] == 0)
    od;

    atomic {
        task_ref_count[TASK_A]--;
        task_ref_count[TASK_B]--;
        any_returned = 1;
    }
}

proctype Caller() {
    do
    :: all_held && any_held -> break
    :: else -> skip
    od;

    atomic {
        task_started[TASK_A] = 1;
        task_started[TASK_B] = 1;
        task_ref_count[TASK_A]--;
        task_ref_count[TASK_B]--;
        caller_released = 1;
    }
}

proctype CancelBeforeStart() {
    atomic {
        assert(task_started[TASK_CANCELLED] == 0);
        task_cancelled[TASK_CANCELLED] = 1;
        task_done[TASK_CANCELLED] = 1;
        cancelled_start_attempted = 1;
        cancelled_start_rejected = 1;
        task_ref_count[TASK_CANCELLED]--;
    }
}

proctype ContextOwner() {
    do
    :: all_tasks_destroyed ->
        assert(all_returned == 1);
        assert(any_returned == 1);
        assert(task_ref_count[TASK_A] == 0);
        assert(task_ref_count[TASK_B] == 0);
        assert(task_ref_count[TASK_CANCELLED] == 0);
        break
    :: else -> skip
    od
}

init {
    atomic {
        task_ref_count[TASK_A] = 1;
        task_ref_count[TASK_B] = 1;
        task_ref_count[TASK_CANCELLED] = 1;

        run TaskRunner(TASK_A);
        run TaskRunner(TASK_B);
        run Cleanup();
        run WhenAll();
        run WhenAny();
        run Caller();
        run CancelBeforeStart();
        run ContextOwner();
    }
}

ltl no_destroy_while_held {
    [] (((all_held && !all_returned) || (any_held && !any_returned)) ->
        (!task_destroyed[TASK_A] && !task_destroyed[TASK_B]))
}

ltl all_returns_after_all_done {
    [] (all_returned -> both_done)
}

ltl any_returns_a_done_task {
    [] (any_returned -> any_winner_done)
}

ltl cancellation_is_prestart_only {
    [] (task_cancelled[TASK_CANCELLED] ->
        (!task_started[TASK_CANCELLED] && task_done[TASK_CANCELLED]))
}

ltl combinators_eventually_return {
    [] ((all_held && any_held) -> <> (all_returned && any_returned))
}
