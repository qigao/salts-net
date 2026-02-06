/*!
The MIT License (MIT)

Copyright (c) 2016 Dmitriy Kubyshkin <dmitriy@kubyshkin.name>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef BDD_FOR_C_H
#define BDD_FOR_C_H

/* C++ compatibility */
#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
  #include <stdio.h>
  #include <Windows.h>
  #include <io.h>
  #define __BDD_IS_ATTY__() _isatty(_fileno(stdout))
#else
  #ifndef _POSIX_C_SOURCE
    /* This definition is required for `fileno` to be defined */
    #define _POSIX_C_SOURCE 200809L
  #endif
  #include <stdio.h>
  #include <unistd.h>
  /* term.h may not be available on all systems */
  #ifdef __has_include
    #if __has_include(<term.h>)
      #include <term.h>
    #endif
  #endif
  #define __BDD_IS_ATTY__() isatty(fileno(stdout))
#endif

#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* C++ has bool built-in, C needs stdbool.h */
#ifndef __cplusplus
  #include <stdbool.h>
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996) // _CRT_SECURE_NO_WARNINGS
#endif

#ifndef BDD_USE_COLOR
#define BDD_USE_COLOR 1
#endif

#ifndef BDD_USE_TAP
#define BDD_USE_TAP 0
#endif

#define __BDD_COLOR_RESET__       "\x1B[0m"
#define __BDD_COLOR_RED__         "\x1B[31m"
#define __BDD_COLOR_GREEN__       "\x1B[32m"
#define __BDD_COLOR_YELLOW__       "\x1B[33m"
#define __BDD_COLOR_BOLD__        "\x1B[1m"  // Bold White
#define __BDD_COLOR_MAGENTA__     "\x1B[35m"

/* Cross-platform high-resolution timer */
static inline double __bdd_get_time_ms__(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)(counter.QuadPart * 1000.0) / frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)(ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0);
#endif
}

typedef struct __bdd_array__ {
    void **values;
    size_t capacity;
    size_t size;
} __bdd_array__;

static inline __bdd_array__ *__bdd_array_create__(void) {
    __bdd_array__ *arr = (__bdd_array__ *)malloc(sizeof(__bdd_array__));
    if (!arr) {
        perror("malloc(array)");
        abort();
    }
    arr->capacity = 4;
    arr->size = 0;
    arr->values = (void **)calloc(arr->capacity, sizeof(void *));
    if (!arr->values) {
        perror("calloc(array->values)");
        free(arr);
        abort();
    }
    return arr;
}

static inline void *__bdd_array_push__(__bdd_array__ *arr, void *item) {
    if (arr->size == arr->capacity) {
        arr->capacity *= 2;
        void **v = (void **)realloc(arr->values, sizeof(void*) * arr->capacity);
        if (!v) {
            perror("realloc(array)");
            abort();
        }
        arr->values = v;
    }
    arr->values[arr->size++] = item;
    return item;
}

static inline void *__bdd_array_last__(__bdd_array__ *arr) {
    if (arr->size == 0) {
        return NULL;
    }
    return arr->values[arr->size - 1];
}

static inline void *__bdd_array_pop__(__bdd_array__ *arr) {
    if (arr->size == 0) {
        return NULL;
    }
    void *result = arr->values[arr->size - 1];
    --arr->size;
    return result;
}

static inline void __bdd_array_free__(__bdd_array__ *arr) {
    free(arr->values);
    free(arr);
}

typedef enum __bdd_node_type__ {
    __BDD_NODE_GROUP__ = 1,
    __BDD_NODE_TEST__ = 2,
    __BDD_NODE_INTERIM__ = 3
} __bdd_node_type__;

typedef enum __bdd_node_flags__ {
  __bdd_node_flags_none__         = 0,
  __bdd_node_flags_focus__        = 1 << 0,
  __bdd_node_flags_skip__         = 1 << 1,
  __bdd_node_flags_expected_fail__ = 1 << 2,
} __bdd_node_flags__;

typedef struct __bdd_test_step__ {
    size_t level;
    int id;
    char *name;
    __bdd_node_type__ type;
    __bdd_node_flags__ flags;
    bool executed;
    bool passed;
    char *failure_message;
    char *failure_location;
    double execution_time_ms;
    char *full_path;  /* Full hierarchical path like "Calculator.should add two numbers" */
} __bdd_test_step__;

typedef struct __bdd_node__ {
    int id;
    int next_node_id;
    char *name;
    __bdd_node_flags__ flags;
    __bdd_node_type__ type;
    __bdd_array__ *list_before;
    __bdd_array__ *list_after;
    __bdd_array__ *list_before_each;
    __bdd_array__ *list_after_each;
    __bdd_array__ *list_children;
} __bdd_node__;

enum __bdd_run_type__ {
    __BDD_INIT_RUN__ = 1,
    __BDD_TEST_RUN__ = 2
};

typedef struct __bdd_config_type__ {
    enum __bdd_run_type__ run;
    int id;
    size_t test_index;
    size_t test_tap_index;
    size_t failed_test_count;
    __bdd_test_step__ *current_test;
    __bdd_array__ *node_stack;
    __bdd_array__ *nodes;
    char *error;
    char *location;
    bool use_color;
    bool use_tap;
    bool has_focus_nodes;
    const char *junit_file;
} __bdd_config_type__;

static inline __bdd_test_step__ *__bdd_test_step_create__(size_t level, __bdd_node__ *node) {
    __bdd_test_step__ *step = (__bdd_test_step__ *)malloc(sizeof(__bdd_test_step__));
    if (!step) {
        perror("malloc(step)");
        abort();
    }
    step->id = node->id;
    step->level = level;
    step->type = node->type;
    step->name = node->name;
    step->flags = node->flags;
    step->executed = false;
    step->passed = false;
    step->failure_message = NULL;
    step->failure_location = NULL;
    step->execution_time_ms = 0.0;
    step->full_path = NULL;
    return step;
}

static inline __bdd_node__ *__bdd_node_create__(int id, const char *name, __bdd_node_type__ type, __bdd_node_flags__ flags) {
    __bdd_node__ *n = (__bdd_node__ *)malloc(sizeof(__bdd_node__));
    if (!n) {
        perror("malloc(node)");
        abort();
    }
    n->id = id;
    n->next_node_id = id + 1;
    n->name = (char *)name; /* node takes ownership of name - cast away const for C++ */
    n->type = type;
    n->flags = flags;
    n->list_before = __bdd_array_create__();
    n->list_after = __bdd_array_create__();
    n->list_before_each = __bdd_array_create__();
    n->list_after_each = __bdd_array_create__();
    n->list_children = __bdd_array_create__();
    return n;
}

static inline bool __bdd_node_is_leaf__(__bdd_node__ *node) {
    return node->list_children->size == 0;
}

static void __bdd_node_flatten_internal__(
    __bdd_config_type__ *config,
    size_t level,
    __bdd_node__ *node,
    __bdd_array__  *steps,
    __bdd_array__  *before_each_lists,
    __bdd_array__  *after_each_lists
) {
    if (__bdd_node_is_leaf__(node)) {
        if (config->has_focus_nodes && !(node->flags & __bdd_node_flags_focus__)) {
            return;
        }

        for (size_t listIndex = 0; listIndex < before_each_lists->size; ++listIndex) {
            __bdd_array__ *list = (__bdd_array__ *)before_each_lists->values[listIndex];
            for (size_t i = 0; i < list->size; ++i) {
                __bdd_array_push__(steps, __bdd_test_step_create__(level, (__bdd_node__ *)list->values[i]));
            }
        }

        __bdd_array_push__(steps, __bdd_test_step_create__(level, node));

        for (size_t listIndex = 0; listIndex < after_each_lists->size; ++listIndex) {
            size_t reverseListIndex = after_each_lists->size - listIndex - 1;
            __bdd_array__ *list = (__bdd_array__ *)after_each_lists->values[reverseListIndex];
            for (size_t i = 0; i < list->size; ++i) {
                __bdd_array_push__(steps, __bdd_test_step_create__(level, (__bdd_node__ *)list->values[i]));
            }
        }
        return;
    }

    __bdd_array_push__(steps, __bdd_test_step_create__(level, node));

    for (size_t i = 0; i < node->list_before->size; ++i) {
        __bdd_array_push__(steps, __bdd_test_step_create__(level + 1, (__bdd_node__ *)node->list_before->values[i]));
    }

    __bdd_array_push__(before_each_lists, node->list_before_each);
    __bdd_array_push__(after_each_lists, node->list_after_each);

    for (size_t i = 0; i < node->list_children->size; ++i) {
        __bdd_node_flatten_internal__(
          config, level + 1, (__bdd_node__ *)node->list_children->values[i], steps, before_each_lists, after_each_lists
        );
    }

    __bdd_array_pop__(before_each_lists);
    __bdd_array_pop__(after_each_lists);

    for (size_t i = 0; i < node->list_after->size; ++i) {
        __bdd_array_push__(steps, __bdd_test_step_create__(level + 1, (__bdd_node__ *)node->list_after->values[i]));
    }
}

static __bdd_array__ *__bdd_node_flatten__(__bdd_config_type__ *config, __bdd_node__ *node, __bdd_array__ *steps) {
    if (node == NULL) {
        return steps;
    }

    __bdd_array__ *before_each_lists = __bdd_array_create__();
    __bdd_array__ *after_each_lists = __bdd_array_create__();
    __bdd_node_flatten_internal__(config, 0, node, steps, before_each_lists, after_each_lists);
    __bdd_array_free__(before_each_lists);
    __bdd_array_free__(after_each_lists);

    return steps;
}

static void __bdd_node_free__(__bdd_node__ *n) {
    free(n->name);
    __bdd_array_free__(n->list_before);
    __bdd_array_free__(n->list_after);
    __bdd_array_free__(n->list_before_each);
    __bdd_array_free__(n->list_after_each);
    __bdd_array_free__(n->list_children);
    free(n);
}

/* Forward declarations - these will be defined by the spec() macro */
#ifdef __cplusplus
extern const char *__bdd_spec_name__;
extern void __bdd_test_main__(__bdd_config_type__ *__bdd_config__);
#else
static char *__bdd_spec_name__;
static void __bdd_test_main__(__bdd_config_type__ *__bdd_config__);
#endif
static char *__bdd_vformat__(const char *format, va_list va);

static void __bdd_indent__(FILE *fp, size_t level) {
    for (size_t i = 0; i < level; ++i) {
        fprintf(fp, "  ");
    }
}

static bool __bdd_enter_node__(__bdd_node_flags__ node_flags, __bdd_config_type__ *config, __bdd_node_type__ type, ptrdiff_t list_offset, const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    char *name = __bdd_vformat__(fmt, va);
    va_end(va);

    if (config->run == __BDD_INIT_RUN__) {
        __bdd_node__ *top = (__bdd_node__ *)__bdd_array_last__(config->node_stack);
        __bdd_array__ *list = *(__bdd_array__ **)((unsigned char *)top + list_offset);

        int id = config->id++;
        __bdd_node__ *node = __bdd_node_create__(id, name, type, node_flags);
        if (node_flags & __bdd_node_flags_focus__) {
            /* Propagate focus to group nodes up the tree to print only them */
            top->flags = (__bdd_node_flags__)(top->flags | (node_flags & __bdd_node_flags_focus__));
            config->has_focus_nodes = true;
        }
        __bdd_array_push__(list, node);
        __bdd_array_push__(config->nodes, node);
        if (type == __BDD_NODE_GROUP__) {
            __bdd_array_push__(config->node_stack, node);
            return true;
        }
        return false;
    }

    if (config->id >= (int)config->nodes->size) {
        fprintf(stderr, "non-deterministic spec\n");
        abort();
    }
    __bdd_node__ *node = (__bdd_node__ *)config->nodes->values[config->id];
    if (node->type != type || strcmp(node->name, name) != 0) {
        fprintf(stderr, "non-deterministic spec\n");
        abort();
    }
    free(name);

    __bdd_test_step__ *step = config->current_test;
    bool should_enter = step->id >= node->id && step->id < node->next_node_id;
    if (should_enter) {
        __bdd_array_push__(config->node_stack, node);
        config->id++;
    } else {
        config->id = node->next_node_id;
    }
#if defined(BDD_PRINT_TRACE)
    const char *color = config->use_color ? __BDD_COLOR_MAGENTA__ : "";
    fprintf(stderr, "%s% 3d ", color, step->id);
    __bdd_indent__(stderr, config->node_stack->size - 1 - (int)should_enter);
    const char *reset = config->use_color ? __BDD_COLOR_RESET__ : "";
    fprintf(stderr,
        "%s [%d, %d) %s%s\n",
        should_enter ? ">" : "|",
        node->id,
        node->next_node_id,
        node->name,
        reset);
#endif
    return should_enter;
}

static void __bdd_exit_node__(__bdd_config_type__ *config) {
    __bdd_node__ *top = (__bdd_node__ *)__bdd_array_pop__(config->node_stack);
    if (config->run == __BDD_INIT_RUN__) {
        top->next_node_id = config->id;
    }
}

static void __bdd_report_skip__(__bdd_config_type__ *config, __bdd_test_step__ *step) {
    if (config->run == __BDD_TEST_RUN__) {
        if (!config->has_focus_nodes) {
            if (config->use_tap) {
                if (config->test_tap_index) {
                    printf("skipped %zu - %s\n", config->test_tap_index, step->name);
                }
            } else {
                printf(
                    "%s(SKIP)%s\n",
                    config->use_color ? __BDD_COLOR_YELLOW__ : "",
                    config->use_color ? __BDD_COLOR_RESET__ : ""
                );
            }
        }
    }
}

static void __bdd_report_pass__(__bdd_config_type__ *config, __bdd_test_step__ *step) {
    if (config->run == __BDD_TEST_RUN__) {
        if (config->use_tap) {
            if (config->test_tap_index) {
                printf("ok %zu - %s\n", config->test_tap_index, step->name);
            }
        } else {
            printf(
                "%s(OK)%s\n",
                config->use_color ? __BDD_COLOR_GREEN__ : "",
                config->use_color ? __BDD_COLOR_RESET__ : ""
            );
        }
    }
}

static void __bdd_report_unexpected_pass__(__bdd_config_type__ *config, __bdd_test_step__ *step) {
    ++config->failed_test_count;
    if (config->run == __BDD_TEST_RUN__) {
        if (config->use_tap) {
            if (config->test_tap_index) {
                printf("not ok %zu - %s # TODO was expected to fail but passed\n", config->test_tap_index, step->name);
            }
        } else {
            printf(
                "%s(UNEXPECTED PASS)%s\n",
                config->use_color ? __BDD_COLOR_YELLOW__ : "",
                config->use_color ? __BDD_COLOR_RESET__ : ""
            );
            __bdd_indent__(stdout, step->level + 1);
            printf("This test was expected to fail but passed\n");
        }
    }
}

static void __bdd_report_expected_fail__(__bdd_config_type__ *config, __bdd_test_step__ *step) {
    if (config->run == __BDD_TEST_RUN__) {
        if (config->use_tap) {
            if (config->test_tap_index) {
                printf("ok %zu - %s # TODO expected failure\n", config->test_tap_index, step->name);
            }
        } else {
            printf(
                "%s(OK - expected fail)%s\n",
                config->use_color ? __BDD_COLOR_GREEN__ : "",
                config->use_color ? __BDD_COLOR_RESET__ : ""
            );
        }
    }
}

static void __bdd_report_fail__(__bdd_config_type__ *config, __bdd_test_step__ *step) {
    ++config->failed_test_count;
    if (config->use_tap) {
        if (config->test_tap_index) {
            printf("not ok %zu - %s\n", config->test_tap_index, step->name);
        }
    } else {
        printf(
            "%s(FAIL)%s\n",
            config->use_color ? __BDD_COLOR_RED__ : "",
            config->use_color ? __BDD_COLOR_RESET__ : ""
        );
        __bdd_indent__(stdout, step->level + 1);
        printf("%s\n", config->error);
        __bdd_indent__(stdout, step->level + 2);
        printf("%s\n", config->location);
    }
}

static void __bdd_run__(__bdd_config_type__ *config) {
    __bdd_test_step__ *step = config->current_test;

    if (step->type == __BDD_NODE_GROUP__ && !config->use_tap) {
        if (config->has_focus_nodes && !(step->flags & __bdd_node_flags_focus__)) {
            return;
        }
        __bdd_indent__(stdout, step->level);
        printf(
            "%s%s%s\n",
            config->use_color ? __BDD_COLOR_BOLD__ : "",
            step->name,
            config->use_color ? __BDD_COLOR_RESET__ : ""
        );
        return;
    }

    bool skipped = false;
    if (step->type == __BDD_NODE_TEST__) {
        if (config->has_focus_nodes && !(step->flags & __bdd_node_flags_focus__)) {
            skipped = true;
        } else if (step->flags & __bdd_node_flags_skip__) {
            skipped = true;
        }
        ++config->test_tap_index;
        
        /* Print the step name before running the test so it is visible even if the test crashes */
        if ((!skipped || !config->has_focus_nodes) && config->run == __BDD_TEST_RUN__ && !config->use_tap) {
            __bdd_indent__(stdout, step->level);
            printf("%s ", step->name);
        }

        if (!skipped) {
            double start_time = __bdd_get_time_ms__();
            __bdd_test_main__(config);
            double end_time = __bdd_get_time_ms__();
            step->execution_time_ms = end_time - start_time;
        }

        if (skipped) {
            __bdd_report_skip__(config, step);
        } else if (config->error == NULL) {
            /* Test passed */
            step->executed = true;
            bool is_expected_fail = (step->flags & __bdd_node_flags_expected_fail__);
            if (is_expected_fail) {
                step->passed = false;
                step->failure_message = strdup("Expected to fail but passed");
                __bdd_report_unexpected_pass__(config, step);
            } else {
                step->passed = true;
                __bdd_report_pass__(config, step);
            }
        } else {
            /* Test failed */
            step->executed = true;
            bool is_expected_fail = (step->flags & __bdd_node_flags_expected_fail__);
            if (is_expected_fail) {
                step->passed = true;  /* Expected failure counts as pass */
                __bdd_report_expected_fail__(config, step);
            } else {
                step->passed = false;
                step->failure_message = strdup(config->error);
                step->failure_location = strdup(config->location);
                __bdd_report_fail__(config, step);
            }
            free(config->error);
            config->error = NULL;
        }
    } else if (!skipped) {
        __bdd_test_main__(config);
    }
}

static char *__bdd_vformat__(const char *format, va_list va) {
    /* First we over-allocate */
    const size_t size = 2048;
    char *result = (char *)calloc(size, sizeof(char));
    if (!result) {
        perror("calloc(result)");
        abort();
    }
    vsnprintf(result, size - 1, format, va);

    /* Then clip to an actual size */
    char *r = (char *)realloc(result, strlen(result) + 1);
    if (!r) {
        perror("realloc(result)");
        abort();
    }
    result = r;
    return result;
}

static char *__bdd_format__(const char *format, ...) {
    va_list va;
    va_start(va, format);
    char *result = __bdd_vformat__(format, va);
    va_end(va);
    return result;
}

static bool __bdd_is_supported_term__(void) {
    bool result;
    const char *term = getenv("TERM");
    result = term && strcmp(term, "") != 0;
#ifndef _WIN32
    return result;
#else
    if (result) {
        return 1;
    }

    // Attempt to enable virtual terminal processing on Windows.
    // See: https://msdn.microsoft.com/en-us/library/windows/desktop/mt638032(v=vs.85).aspx
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) {
        return 0;
    }

    dwMode |= 0x4; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    if (!SetConsoleMode(hOut, dwMode)) {
        return 0;
    }

    return 1;
#endif
}

static void __bdd_xml_escape__(FILE *f, const char *str) {
    if (!str) return;
    for (const char *p = str; *p; ++p) {
        switch (*p) {
            case '&':  fprintf(f, "&amp;"); break;
            case '<':  fprintf(f, "&lt;"); break;
            case '>':  fprintf(f, "&gt;"); break;
            case '"':  fprintf(f, "&quot;"); break;
            case '\'': fprintf(f, "&apos;"); break;
            default:   fputc(*p, f); break;
        }
    }
}

static void __bdd_generate_junit__(__bdd_config_type__ *config, __bdd_array__ *steps, size_t test_count) {
    FILE *f = fopen(config->junit_file, "w");
    if (!f) {
        fprintf(stderr, "Error: Could not open JUnit output file: %s\n", config->junit_file);
        return;
    }

    /* Get current timestamp in ISO 8601 format */
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", tm_info);

    /* Count skipped tests */
    size_t skipped_count = 0;
    for (size_t i = 0; i < steps->size; ++i) {
        __bdd_test_step__ *step = (__bdd_test_step__ *)steps->values[i];
        if (step->type == __BDD_NODE_TEST__ && (step->flags & __bdd_node_flags_skip__)) {
            ++skipped_count;
        }
    }

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<testsuites name=\"BDD Test Run\" tests=\"%zu\" failures=\"%zu\" errors=\"0\" skipped=\"%zu\" time=\"0\" timestamp=\"%s\">\n",
            test_count, config->failed_test_count, skipped_count, timestamp);
    
    fprintf(f, "  <testsuite name=\"");
    __bdd_xml_escape__(f, __bdd_spec_name__);
    fprintf(f, "\" tests=\"%zu\" failures=\"%zu\" errors=\"0\" skipped=\"%zu\" time=\"0\" timestamp=\"%s\">\n", 
            test_count, config->failed_test_count, skipped_count, timestamp);

    for (size_t i = 0; i < steps->size; ++i) {
        __bdd_test_step__ *step = (__bdd_test_step__ *)steps->values[i];
        if (step->type == __BDD_NODE_TEST__) {
            fprintf(f, "    <testcase name=\"");
            __bdd_xml_escape__(f, step->name);
            fprintf(f, "\" classname=\"");
            /* Use full hierarchical path if available, otherwise use spec name */
            if (step->full_path) {
                __bdd_xml_escape__(f, step->full_path);
            } else {
                __bdd_xml_escape__(f, __bdd_spec_name__);
            }
            fprintf(f, "\" time=\"%.6f\"", step->execution_time_ms / 1000.0);  /* Convert ms to seconds */
            
            bool is_skip = (step->flags & __bdd_node_flags_skip__);
            
            if (is_skip) {
                fprintf(f, ">\n");
                fprintf(f, "      <skipped message=\"Test was skipped\" />\n");
                fprintf(f, "    </testcase>\n");
            } else if (step->executed && !step->passed) {
                /* Test failed */
                fprintf(f, ">\n");
                fprintf(f, "      <failure message=\"");
                if (step->failure_message) {
                    __bdd_xml_escape__(f, step->failure_message);
                } else {
                    fprintf(f, "Test failed");
                }
                fprintf(f, "\" type=\"AssertionError\">");
                if (step->failure_location) {
                    fprintf(f, "\n");
                    __bdd_xml_escape__(f, step->failure_location);
                }
                if (step->failure_message) {
                    fprintf(f, "\n");
                    __bdd_xml_escape__(f, step->failure_message);
                }
                fprintf(f, "\n      </failure>\n");
                fprintf(f, "    </testcase>\n");
            } else {
                /* Test passed */
                fprintf(f, " />\n");
            }
        }
    }

    fprintf(f, "  </testsuite>\n");
    fprintf(f, "</testsuites>\n");
    fclose(f);
}

#ifdef __cplusplus
}
#endif


/* main() must not be in extern "C" block */
int main(int argc, char **argv) {
    double __bdd_start_time__ = __bdd_get_time_ms__();
    struct __bdd_config_type__ config;
    config.run = __BDD_INIT_RUN__;
    config.id = 0;
    config.test_index = 0;
    config.test_tap_index = 0;
    config.failed_test_count = 0;
    config.node_stack = __bdd_array_create__();
    config.nodes = __bdd_array_create__();
    config.error = NULL;
    config.location = NULL;
    config.current_test = NULL;
    config.use_color = 0;
    config.use_tap = 0;
    config.has_focus_nodes = 0;
    config.junit_file = NULL;

    /* Parse command-line arguments */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--tap") == 0) {
            config.use_tap = 1;
        } else if (strcmp(argv[i], "--color") == 0) {
            config.use_color = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            config.use_color = 0;
        } else if (strcmp(argv[i], "--junit") == 0) {
            if (i + 1 < argc) {
                config.junit_file = argv[++i];
            } else {
                fprintf(stderr, "Error: --junit requires a filename argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --tap              Output in TAP (Test Anything Protocol) format\n");
            printf("  --color            Force colored output\n");
            printf("  --no-color         Disable colored output\n");
            printf("  --junit <file>     Generate JUnit XML report to specified file\n");
            printf("  --help, -h         Show this help message\n");
            return 0;
        }
    }

    const char *tap_env = getenv("BDD_USE_TAP");
    if (!config.use_tap && (BDD_USE_TAP || (tap_env && strcmp(tap_env, "") != 0 && strcmp(tap_env, "0") != 0))) {
        config.use_tap = 1;
    }

    if (!config.use_tap && !config.use_color && BDD_USE_COLOR && __BDD_IS_ATTY__() && __bdd_is_supported_term__()) {
        config.use_color = 1;
    }

    __bdd_node__ *root = __bdd_node_create__(-1, __bdd_spec_name__, __BDD_NODE_GROUP__, __bdd_node_flags_none__);
    __bdd_array_push__(config.node_stack, root);

    // During the first run we just gather the
    // count of the tests and their descriptions
    __bdd_test_main__(&config);

    __bdd_array__ *steps = __bdd_array_create__();
    __bdd_node_flatten__(&config, root, steps);

    /* Build full hierarchical paths for test cases */
    char path_buffer[1024];
    __bdd_test_step__ *group_stack[32];  /* Stack to track describe blocks */
    int stack_depth = 0;
    
    for (size_t i = 0; i < steps->size; ++i) {
        __bdd_test_step__ *step = (__bdd_test_step__ *)steps->values[i];
        
        /* Maintain stack based on level changes */
        while (stack_depth > 0 && group_stack[stack_depth - 1]->level >= step->level) {
            stack_depth--;
        }
        
        if (step->type == __BDD_NODE_GROUP__) {
            /* Push describe block onto stack */
            if (stack_depth < 32) {
                group_stack[stack_depth++] = step;
            }
        } else if (step->type == __BDD_NODE_TEST__) {
            /* Build full path from stack (skip root spec at index 0) */
            path_buffer[0] = '\0';
            for (int j = 1; j < stack_depth; ++j) {
                if (j > 1) strcat(path_buffer, ".");
                strcat(path_buffer, group_stack[j]->name);
            }
            if (path_buffer[0] != '\0') {
                step->full_path = strdup(path_buffer);
            }
        }
    }

    size_t test_count = 0;
    for (size_t i = 0; i < steps->size; ++i) {
        __bdd_test_step__ *step = (__bdd_test_step__ *)steps->values[i];
        if(step->type == __BDD_NODE_TEST__) {
            ++test_count;
        }
    }

    // Outputting the name of the suite
    if (config.use_tap) {
        printf("TAP version 13\n1..%zu\n", test_count);
    }

    config.run = __BDD_TEST_RUN__;

    for (size_t i = 0; i < steps->size; ++i) {
        __bdd_test_step__ *step = (__bdd_test_step__ *)steps->values[i];
        config.node_stack->size = 1;
        config.id = 0;
        config.current_test = step;
        __bdd_run__(&config);
    }

    double __bdd_end_time__ = __bdd_get_time_ms__();
    double __bdd_duration__ = (__bdd_end_time__ - __bdd_start_time__) / 1000.0;

    size_t passed_count = 0;
    size_t skipped_count = 0;
    size_t todo_count = 0;
    for (size_t i = 0; i < steps->size; ++i) {
        __bdd_test_step__ *step = (__bdd_test_step__ *)steps->values[i];
        if (step->type != __BDD_NODE_TEST__) continue;
        if (step->flags & __bdd_node_flags_skip__) {
            skipped_count++;
        } else if (step->flags & __bdd_node_flags_expected_fail__) {
            todo_count++;
        } else if (step->passed && step->executed) {
            passed_count++;
        }
    }

    if (!config.use_tap) {
        const char *c_rst = config.use_color ? __BDD_COLOR_RESET__ : "";
        const char *c_red = config.use_color ? __BDD_COLOR_RED__ : "";
        const char *c_grn = config.use_color ? __BDD_COLOR_GREEN__ : "";
        const char *c_ylw = config.use_color ? __BDD_COLOR_YELLOW__ : "";
        const char *c_bld = config.use_color ? __BDD_COLOR_BOLD__ : "";

        printf("\n==All Tests Summary==\n");
        printf("Total tests %s[PASSED]:\t%zu%s\n", c_grn, passed_count, c_rst);
        printf("Total tests %s[FAILED]:\t%zu%s\n", c_red, config.failed_test_count, c_rst);
        printf("Total tests %s[SKIPPED]:\t%zu%s\n", c_ylw, skipped_count, c_rst);
        printf("Total tests %s[TODO]:   \t%zu%s\n", c_bld, todo_count, c_rst);

        printf("%zu passed, %zu failed, %zu skipped, %zu todo. Finished in %f sec.\n",
               passed_count, config.failed_test_count, skipped_count, todo_count, __bdd_duration__);

        if (config.failed_test_count == 0 && todo_count == 0) {
            printf("%sAll tests passed in %f sec.%s\n\n", c_grn, __bdd_duration__, c_rst);
        } else {
            printf("\n");
        }
    }

    /* Generate JUnit XML report if requested - must be done before freeing steps */
    if (config.junit_file) {
        __bdd_generate_junit__(&config, steps, test_count);
    }

    for (size_t i = 0; i < config.nodes->size; ++i) {
        __bdd_node_free__((__bdd_node__ *)config.nodes->values[i]);
    }
    root->name = NULL; /* name is statically allocated */
    __bdd_node_free__(root);
    for (size_t i = 0; i < steps->size; ++i) {
        free(steps->values[i]);
    }
    __bdd_array_free__(config.nodes);
    __bdd_array_free__(config.node_stack);
    __bdd_array_free__(steps);
    
    return config.failed_test_count > 0 ? 1 : 0;
}

#ifdef __cplusplus
#define spec(name) \
const char *__bdd_spec_name__ = (name);\
void __bdd_test_main__ (__bdd_config_type__ *__bdd_config__)\

#else
#define spec(name) \
static char *__bdd_spec_name__ = (char *)(name);\
static void __bdd_test_main__ (__bdd_config_type__ *__bdd_config__)\

#endif

#define __BDD_NODE__(flags, node_list, type, ...)\
for(\
    bool __bdd_has_run__ = 0;\
    (\
      !__bdd_has_run__ && \
      __bdd_enter_node__(flags, __bdd_config__, (type), offsetof(struct __bdd_node__, node_list), __VA_ARGS__) \
    );\
    __bdd_exit_node__(__bdd_config__), \
    __bdd_has_run__ = 1 \
)

#define describe(...) __BDD_NODE__(__bdd_node_flags_none__, list_children, __BDD_NODE_GROUP__, __VA_ARGS__)
#define it(...)            __BDD_NODE__(__bdd_node_flags_none__, list_children, __BDD_NODE_TEST__, __VA_ARGS__)
#define it_only(...)       __BDD_NODE__(__bdd_node_flags_focus__, list_children, __BDD_NODE_TEST__, __VA_ARGS__)
#define fit(...)           it_only(__VA_ARGS__)
#define it_skip(...)       __BDD_NODE__(__bdd_node_flags_skip__, list_children, __BDD_NODE_TEST__, __VA_ARGS__)
#define xit(...)           it_skip(__VA_ARGS__)
#define it_should_fail(...) __BDD_NODE__(__bdd_node_flags_expected_fail__, list_children, __BDD_NODE_TEST__, __VA_ARGS__)
#define before_each() __BDD_NODE__(__bdd_node_flags_none__, list_before_each, __BDD_NODE_INTERIM__, "before_each")
#define after_each()  __BDD_NODE__(__bdd_node_flags_none__, list_after_each, __BDD_NODE_INTERIM__, "after_each")

/* Note: before() and after() can conflict with C++ standard library (e.g., std::type_info::before)
 * In C++ code, use before_each() and after_each() instead, or define BDD_USE_BEFORE_AFTER
 * before including this header if you really need them */
#if !defined(__cplusplus) || defined(BDD_USE_BEFORE_AFTER)
#define before()      __BDD_NODE__(__bdd_node_flags_none__, list_before, __BDD_NODE_INTERIM__, "before")
#define after()       __BDD_NODE__(__bdd_node_flags_none__, list_after, __BDD_NODE_INTERIM__, "after")
#endif

#ifndef BDD_NO_CONTEXT_KEYWORD
#define context(name) describe(name)
#endif

#define __BDD_MACRO__(M, ...) __BDD_OVERLOAD__(M, __BDD_COUNT_ARGS__(__VA_ARGS__)) (__VA_ARGS__)
#define __BDD_OVERLOAD__(macro_name, suffix) __BDD_EXPAND_OVERLOAD__(macro_name, suffix)
#define __BDD_EXPAND_OVERLOAD__(macro_name, suffix) macro_name##suffix

#define __BDD_COUNT_ARGS__(...) __BDD_PATTERN_MATCH__(__VA_ARGS__,_,_,_,_,_,_,_,_,_,ONE__)
#define __BDD_PATTERN_MATCH__(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,N, ...) N

#define __BDD_STRING_HELPER__(x) #x
#define __BDD_STRING__(x) __BDD_STRING_HELPER__(x)
#define __STRING__LINE__ __BDD_STRING__(__LINE__)

#define __BDD_FMT_COLOR__ __BDD_COLOR_RED__ "Check failed:" __BDD_COLOR_RESET__ " %s"
#define __BDD_FMT_PLAIN__ "Check failed: %s"

#define __BDD_CHECK__(condition, ...) if (!(condition))\
{\
    char *message = __bdd_format__(__VA_ARGS__);\
    const char *fmt = __bdd_config__->use_color ? __BDD_FMT_COLOR__ : __BDD_FMT_PLAIN__;\
    __bdd_config__->location = (char *)"at " __FILE__ ":" __STRING__LINE__;\
    size_t bufflen = strlen(fmt) + strlen(message) + 1;\
    __bdd_config__->error = (char *)calloc(bufflen, sizeof(char));\
    if (__bdd_config__->use_color) {\
      snprintf(__bdd_config__->error, bufflen, __BDD_FMT_COLOR__, message);\
    } else {\
      snprintf(__bdd_config__->error, bufflen, __BDD_FMT_PLAIN__, message);\
    }\
    free(message);\
    return;\
}

#define __BDD_CHECK_ONE__(condition) __BDD_CHECK__(condition, #condition)

#define check(...) __BDD_MACRO__(__BDD_CHECK_, __VA_ARGS__)

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif /*BDD_FOR_C_H*/
