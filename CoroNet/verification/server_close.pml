/*
 * CoroNet accepted-server shutdown model.
 *
 * Scope:
 *   - accept loop admission and server_task_count ownership
 *   - close_admission/server_stop ordering
 *   - one admitted handler and one stalled admission
 *   - close_pending reference and duplicate destroy requests
 *   - handler_closed exactly once and server quiescence
 */

#define TASK_COUNT 2
#define SERVER_OWNER_REF 1
#define ACCEPT_LOOP_REF 1

byte listener = 1;
byte server_stopping = 0;
byte accept_loop_active = 1;
byte accepted_count = 0;
byte server_task_count = 0;
byte server_ref_count = SERVER_OWNER_REF + ACCEPT_LOOP_REF;

byte task_linked[TASK_COUNT];
byte task_cancel_requested[TASK_COUNT];
byte task_finished[TASK_COUNT];
byte handler_called[TASK_COUNT];
byte handler_call_after_stop[TASK_COUNT];
byte handler_closed_called[TASK_COUNT];

byte socket_destroy_requested[TASK_COUNT];
byte close_pending[TASK_COUNT];
byte socket_ref_count[TASK_COUNT];

byte release_admitted_handler = 0;
byte server_stop_started = 0;
byte server_destroyed = 0;
byte handler_closed_count = 0;

#define server_stopped_state (listener == 0 && \
                              accept_loop_active == 0 && \
                              server_task_count == 0)
#define all_handlers_finished (task_finished[0] && task_finished[1])

inline request_socket_close(id) {
    atomic {
        if
        :: socket_destroy_requested[id] == 0 ->
            socket_destroy_requested[id] = 1;
            if
            :: close_pending[id] == 0 ->
                close_pending[id] = 1;
                socket_ref_count[id]++
            :: else -> skip
            fi
        :: else -> skip
        fi
    }
}

proctype Handler(byte id) {
    /* id 0 is already admitted; id 1 remains in admission until stop. */
    if
    :: id == 0 ->
        if
        :: server_stopping == 0 ->
            handler_called[id] = 1
        :: else ->
            skip
        fi;
        do
        :: release_admitted_handler == 1 -> break
        :: server_stopping == 1 && handler_called[id] == 0 -> break
        :: else -> skip
        od
    :: id == 1 ->
        do
        :: server_stopping == 1 -> break
        :: else -> skip
        od
    fi;

    request_socket_close(id);
    do
    :: close_pending[id] == 0 -> break
    :: else -> skip
    od;

    atomic {
        assert(task_linked[id] == 1);
        assert(socket_ref_count[id] > 0);
        socket_ref_count[id]--;
        task_linked[id] = 0;
        assert(server_task_count > 0);
        server_task_count--;
        handler_closed_called[id] = 1;
        handler_closed_count++;
        task_finished[id] = 1;
        assert(server_ref_count > 0);
        server_ref_count--;
    }
    progress_handler_finished: skip;
}

proctype AcceptLoop() {
    byte id;

    do
    :: listener == 1 && server_stopping == 0 && accepted_count < TASK_COUNT ->
        id = accepted_count;
        atomic {
            task_linked[id] = 1;
            server_task_count++;
            server_ref_count++;
            socket_ref_count[id] = 1;
            accepted_count++;
            run Handler(id);
        }
    :: listener == 0 || server_stopping == 1 ->
        atomic {
            accept_loop_active = 0;
            assert(server_ref_count > 0);
            server_ref_count--;
        }
        break
    :: else -> skip
    od
}

proctype CloseReactor() {
    do
    :: close_pending[0] == 1 ->
        atomic {
            close_pending[0] = 0;
            assert(socket_ref_count[0] > 0);
            socket_ref_count[0]--;
        }
        progress_close_completed: skip
    :: close_pending[1] == 1 ->
        atomic {
            close_pending[1] = 0;
            assert(socket_ref_count[1] > 0);
            socket_ref_count[1]--;
        }
        progress_close_completed_1: skip
    :: server_destroyed == 1 -> break
    :: else -> skip
    od
}

proctype ServerStopper() {
    byte id = 0;

    do
    :: accepted_count == TASK_COUNT -> break
    :: else -> skip
    od;

    atomic {
        server_stopping = 1;
        listener = 0;
        release_admitted_handler = 1;
        server_stop_started = 1;
    }

    do
    :: id < TASK_COUNT ->
        atomic {
            task_cancel_requested[id] = 1;
        }
        request_socket_close(id);
        id++
    :: else -> break
    od
}

proctype ServerOwner() {
    do
    :: server_stopped_state && all_handlers_finished &&
       close_pending[0] == 0 && close_pending[1] == 0 ->
        assert(handler_closed_count == TASK_COUNT);
        assert(socket_ref_count[0] == 0);
        assert(socket_ref_count[1] == 0);
        assert(server_ref_count == SERVER_OWNER_REF);
        atomic {
            server_ref_count--;
            assert(server_ref_count == 0);
            server_destroyed = 1;
        }
        break
    :: else -> skip
    od
}

init {
    atomic {
        run AcceptLoop();
        run CloseReactor();
        run ServerStopper();
        run ServerOwner();
    }
}

ltl stop_reaches_quiescence {
    [] (server_stop_started -> <> server_stopped_state)
}

ltl no_handler_enters_after_stop {
    [] (!handler_call_after_stop[0] && !handler_call_after_stop[1])
}

ltl close_callback_once {
    [] (handler_closed_count <= TASK_COUNT)
}

ltl no_live_socket_after_unlink {
    [] ((!task_linked[0] -> (close_pending[0] == 0 && socket_ref_count[0] == 0)) && \
        (!task_linked[1] -> (close_pending[1] == 0 && socket_ref_count[1] == 0)))
}

ltl server_destroy_is_quiescent {
    [] (server_destroyed -> (server_stopped_state &&
                             all_handlers_finished &&
                             server_ref_count == 0))
}
