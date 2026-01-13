/**
 * @file js_process.c
 * @brief Process spawning bindings for QuickJS using libuv
 */
#include "js_internal.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>

typedef struct JSProcessSpawn {
    uv_process_t process;
    uv_pipe_t stdin_pipe;
    uv_pipe_t stdout_pipe;
    uv_pipe_t stderr_pipe;
    JSContext *ctx;
    JSTurboPromise promise;
    JSTurboByteBuffer stdout_buf;
    JSTurboByteBuffer stderr_buf;
    int64_t exit_code;
    int term_signal;
    int exited;
    int stdout_closed;
    int stderr_closed;
} JSProcessSpawn;

static void js_process_maybe_resolve(JSProcessSpawn *ps) {
    if (!ps->exited || !ps->stdout_closed || !ps->stderr_closed) return;

    JSValue result = JS_NewObject(ps->ctx);
    JS_SetPropertyStr(ps->ctx, result, "exitCode", JS_NewInt64(ps->ctx, ps->exit_code));
    JS_SetPropertyStr(ps->ctx, result, "signal", JS_NewInt32(ps->ctx, ps->term_signal));

    if (ps->stdout_buf.length > 0) {
        JS_SetPropertyStr(ps->ctx, result, "stdout",
            JS_NewStringLen(ps->ctx, (char *)ps->stdout_buf.data, ps->stdout_buf.length));
    } else {
        JS_SetPropertyStr(ps->ctx, result, "stdout", JS_NewString(ps->ctx, ""));
    }

    if (ps->stderr_buf.length > 0) {
        JS_SetPropertyStr(ps->ctx, result, "stderr",
            JS_NewStringLen(ps->ctx, (char *)ps->stderr_buf.data, ps->stderr_buf.length));
    } else {
        JS_SetPropertyStr(ps->ctx, result, "stderr", JS_NewString(ps->ctx, ""));
    }

    js_turbo_promise_resolve(&ps->promise, result);

    js_turbo_buffer_free(&ps->stdout_buf);
    js_turbo_buffer_free(&ps->stderr_buf);
    free(ps);
}

static void js_process_exit_cb(uv_process_t *process, int64_t exit_status, int term_signal) {
    JSProcessSpawn *ps = process->data;
    if (!ps) return;

    ps->exit_code = exit_status;
    ps->term_signal = term_signal;
    ps->exited = 1;

    uv_close((uv_handle_t *)process, NULL);
    js_process_maybe_resolve(ps);
}

static void js_process_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len = buf->base ? (unsigned long)suggested_size : 0;
}

static void js_process_stdout_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    JSProcessSpawn *ps = stream->data;

    if (nread > 0 && ps) {
        js_turbo_buffer_append(&ps->stdout_buf, (uint8_t *)buf->base, nread);
    }

    if (buf->base) free(buf->base);

    if (nread < 0) {
        if (ps) {
            ps->stdout_closed = 1;
            uv_close((uv_handle_t *)stream, NULL);
            js_process_maybe_resolve(ps);
        }
    }
}

static void js_process_stderr_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    JSProcessSpawn *ps = stream->data;

    if (nread > 0 && ps) {
        js_turbo_buffer_append(&ps->stderr_buf, (uint8_t *)buf->base, nread);
    }

    if (buf->base) free(buf->base);

    if (nread < 0) {
        if (ps) {
            ps->stderr_closed = 1;
            uv_close((uv_handle_t *)stream, NULL);
            js_process_maybe_resolve(ps);
        }
    }
}

static JSValue js_proc_spawn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "spawn requires options object");
    }

    JSTurboContextState *state = js_turbo_get_state(ctx);
    if (!state || !state->loop) {
        return JS_ThrowInternalError(ctx, "Event loop not initialized");
    }

    JSValue options = argv[0];

    // Get file (required)
    JSValue file_val = JS_GetPropertyStr(ctx, options, "file");
    if (!JS_IsString(file_val)) {
        JS_FreeValue(ctx, file_val);
        return JS_ThrowTypeError(ctx, "spawn requires 'file' property");
    }
    const char *file = JS_ToCString(ctx, file_val);
    JS_FreeValue(ctx, file_val);
    if (!file) return JS_EXCEPTION;

    // Get args (optional)
    JSValue args_val = JS_GetPropertyStr(ctx, options, "args");
    char **args = NULL;
    int args_count = 0;

    if (JS_IsArray(args_val)) {
        int64_t len;
        JS_GetLength(ctx, args_val, &len);
        args_count = (int)len;
        args = calloc(args_count + 1, sizeof(char *));
        for (int i = 0; i < args_count; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, args_val, i);
            args[i] = (char *)JS_ToCString(ctx, item);
            JS_FreeValue(ctx, item);
        }
        args[args_count] = NULL;
    } else {
        // Default: just the file as argv[0]
        args = calloc(2, sizeof(char *));
        args[0] = (char *)file;
        args[1] = NULL;
        args_count = 1;
    }
    JS_FreeValue(ctx, args_val);

    // Get cwd (optional)
    JSValue cwd_val = JS_GetPropertyStr(ctx, options, "cwd");
    const char *cwd = NULL;
    if (JS_IsString(cwd_val)) {
        cwd = JS_ToCString(ctx, cwd_val);
    }
    JS_FreeValue(ctx, cwd_val);

    // Create process spawn structure
    JSProcessSpawn *ps = calloc(1, sizeof(JSProcessSpawn));
    if (!ps) {
        JS_FreeCString(ctx, file);
        if (cwd) JS_FreeCString(ctx, cwd);
        for (int i = 0; i < args_count; i++) {
            if (args[i] != file) JS_FreeCString(ctx, args[i]);
        }
        free(args);
        return JS_ThrowOutOfMemory(ctx);
    }

    ps->ctx = ctx;
    js_turbo_buffer_init(&ps->stdout_buf);
    js_turbo_buffer_init(&ps->stderr_buf);

    // Initialize pipes
    uv_pipe_init(state->loop, &ps->stdin_pipe, 0);
    uv_pipe_init(state->loop, &ps->stdout_pipe, 0);
    uv_pipe_init(state->loop, &ps->stderr_pipe, 0);

    ps->stdout_pipe.data = ps;
    ps->stderr_pipe.data = ps;

    // Setup stdio
    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[1].data.stream = (uv_stream_t *)&ps->stdout_pipe;
    stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[2].data.stream = (uv_stream_t *)&ps->stderr_pipe;

    // Setup process options
    uv_process_options_t proc_opts = {0};
    proc_opts.exit_cb = js_process_exit_cb;
    proc_opts.file = file;
    proc_opts.args = args;
    proc_opts.cwd = cwd;
    proc_opts.stdio_count = 3;
    proc_opts.stdio = stdio;

    // Create promise
    JSValue promise;
    if (js_turbo_promise_init(ctx, &ps->promise, &promise) < 0) {
        JS_FreeCString(ctx, file);
        if (cwd) JS_FreeCString(ctx, cwd);
        for (int i = 0; i < args_count; i++) {
            if (args[i] != file) JS_FreeCString(ctx, args[i]);
        }
        free(args);
        js_turbo_buffer_free(&ps->stdout_buf);
        js_turbo_buffer_free(&ps->stderr_buf);
        free(ps);
        return JS_EXCEPTION;
    }

    // Spawn process
    ps->process.data = ps;
    int rc = uv_spawn(state->loop, &ps->process, &proc_opts);

    // Cleanup strings
    JS_FreeCString(ctx, file);
    if (cwd) JS_FreeCString(ctx, cwd);
    for (int i = 0; i < args_count; i++) {
        if (args[i] && args[i] != file) JS_FreeCString(ctx, args[i]);
    }
    free(args);

    if (rc != 0) {
        js_turbo_promise_reject_message(&ps->promise, uv_strerror(rc));
        js_turbo_buffer_free(&ps->stdout_buf);
        js_turbo_buffer_free(&ps->stderr_buf);
        free(ps);
        return promise;
    }

    // Start reading stdout/stderr
    uv_read_start((uv_stream_t *)&ps->stdout_pipe, js_process_alloc_cb, js_process_stdout_read_cb);
    uv_read_start((uv_stream_t *)&ps->stderr_pipe, js_process_alloc_cb, js_process_stderr_read_cb);

    return promise;
}

static JSValue js_proc_kill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "kill requires pid argument");

    int32_t pid;
    if (JS_ToInt32(ctx, &pid, argv[0]) < 0) return JS_EXCEPTION;

    int signum = SIGTERM;
    if (argc > 1) {
        if (JS_ToInt32(ctx, &signum, argv[1]) < 0) return JS_EXCEPTION;
    }

    int rc = uv_kill(pid, signum);
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "Failed to kill process: %s", uv_strerror(rc));
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_proc_funcs[] = {
    JS_CFUNC_DEF("spawn", 1, js_proc_spawn),
    JS_CFUNC_DEF("kill", 2, js_proc_kill),
};

int js_turbo_register_proc(JSContext *ctx, JSValue turbo_obj) {
    JSValue proc_obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proc_obj, js_proc_funcs, countof(js_proc_funcs));
    JS_SetPropertyStr(ctx, turbo_obj, "proc", proc_obj);
    return 0;
}
