/**
 * @file exprtk_builtins.c
 * @brief Built-in function dispatch for exprtk
 *
 * Uses a sorted lookup table with binary search for O(log n) dispatch
 * instead of a linear if/else strcmp chain. Arena allocations use
 * ALLOC_DBL from arena_buffer.h for type-safe array allocation.
 */

#include "exprtk_dispatch.h"
#include "csv_parser.h"
#include "datetime_parser.h"
#include "turbo_fs.h"
#include "turbo_str.h"
#include "turbo_str_view.h"

exprtk_value_t exprtk_call_internal(const char *name, size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    exprtk_value_t zero = { exprtk_VAL_NUMBER, {0.0} };
    exprtk_value_t result = zero;

    /* 1. Check native/script functions in environment */
    if (env) {
        exprtk_env_t *curr_env_iter = env;
        while (curr_env_iter) {
            exprtk_func_t *f = curr_env_iter->funcs;
            while (f) {
                if (strcmp(f->name, name) == 0) {
                    if (f->is_script) {
                        if (argc != f->data.script.arg_count) {
                            return zero;
                        }
                        env->curr_recursion++;
                        if (env->curr_recursion > env->max_recursion) {
                            env->aborted = 1;
                            env->curr_recursion--;
                            return zero;
                        }

                        exprtk_env_t local_env;
                        exprtk_env_init(&local_env);
                        local_env.parent = env;
                        local_env.max_recursion = env->max_recursion;
                        local_env.curr_recursion = env->curr_recursion;
                        local_env.max_loop_iterations = env->max_loop_iterations;
                        local_env.curr_loop_iterations = env->curr_loop_iterations;
                        local_env.max_nodes = env->max_nodes;
                        local_env.curr_nodes = env->curr_nodes;

                        for (size_t i = 0; i < argc; ++i) {
                            exprtk_env_set(&local_env, f->data.script.arg_names[i], args[i]);
                        }
                        result = exprtk_eval(f->data.script.body, &local_env);

                        env->curr_nodes = local_env.curr_nodes;
                        env->curr_loop_iterations = local_env.curr_loop_iterations;
                        env->aborted = local_env.aborted;
                        env->curr_recursion--;

                        exprtk_env_free(&local_env);
                        return result;
                    } else {
                        return f->data.native.fn(argc, args, f->data.native.user_data);
                    }
                }
                f = f->next;
            }
            curr_env_iter = curr_env_iter->parent;
        }
    }

    /* 2. Built-in functions — O(log n) sorted-table lookup */
    int id = exprtk_dispatch_find(name);
    if (id < 0) return result;

    switch (id) {

    case BI_SIN: if (argc == 1) result = exprtk_val_num(sin(args[0].data.number)); break;
    case BI_COS: if (argc == 1) result = exprtk_val_num(cos(args[0].data.number));
    break;
    case BI_TAN: if (argc == 1) result = exprtk_val_num(tan(args[0].data.number));
    break;
    case BI_SQRT: if (argc == 1) result = exprtk_val_num(sqrt(args[0].data.number));
    break;
    case BI_ABS: if (argc == 1) result = exprtk_val_num(fabs(args[0].data.number));
    break;
    case BI_EXP: if (argc == 1) result = exprtk_val_num(exp(args[0].data.number));
    break;
    case BI_LOG: if (argc == 1) result = exprtk_val_num(log(args[0].data.number));
    break;
    case BI_CEIL: if (argc == 1) result = exprtk_val_num(ceil(args[0].data.number));
    break;
    case BI_FLOOR: if (argc == 1) result = exprtk_val_num(floor(args[0].data.number));
    break;
    case BI_ROUND: if (argc == 1) result = exprtk_val_num(round(args[0].data.number));
    break;
    case BI_LEN: case BI_SIZE: if (argc == 1) {
        if (args[0].type == exprtk_VAL_STRING) result = exprtk_val_num((double)args[0].data.string.len);
        else if (args[0].type == exprtk_VAL_VECTOR) result = exprtk_val_num((double)args[0].data.vector.size);
        else result = exprtk_val_num(0.0);
    }
    break;
    case BI_MIN: if (argc > 0) {
        double res = args[0].data.number;
        for (size_t i = 1; i < argc; ++i) if (args[i].data.number < res) res = args[i].data.number;
        result = exprtk_val_num(res);
    }
    break;
    case BI_MAX: if (argc > 0) {
        double res = args[0].data.number;
        for (size_t i = 1; i < argc; ++i) if (args[i].data.number > res) res = args[i].data.number;
        result = exprtk_val_num(res);
    }
    break;
    case BI_AVG: if (argc > 0) {
        double sum = 0;
        for (size_t i = 0; i < argc; ++i) sum += args[i].data.number;
        result = exprtk_val_num(sum / (double)argc);
    }
    break;
    case BI_SUM: if (argc > 0) {
        double sum = 0;
        for (size_t i = 0; i < argc; ++i) sum += args[i].data.number;
        result = exprtk_val_num(sum);
    }
    break;
    case BI_LOWER: if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        tstr_v s = args[0].data.string;
        char *buf = turbo_arena_alloc(arena, s.len);
        if (buf) {
            for (size_t i = 0; i < s.len; ++i) buf[i] = (char)tolower((unsigned char)s.data[i]);
            result = exprtk_val_str(tstr_v_from_buf(buf, s.len));
        }
    }
    break;
    case BI_UPPER: if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        tstr_v s = args[0].data.string;
        char *buf = turbo_arena_alloc(arena, s.len);
        if (buf) {
            for (size_t i = 0; i < s.len; ++i) buf[i] = (char)toupper((unsigned char)s.data[i]);
            result = exprtk_val_str(tstr_v_from_buf(buf, s.len));
        }
    }
    break;
    case BI_TRIM: if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        result = exprtk_val_str(tstr_v_trim(args[0].data.string, " \t\r\n"));
    }
    break;
    case BI_LTRIM: if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        result = exprtk_val_str(tstr_v_trim_left(args[0].data.string, " \t\r\n"));
    }
    break;
    case BI_RTRIM: if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        result = exprtk_val_str(tstr_v_trim_right(args[0].data.string, " \t\r\n"));
    }
    break;
    case BI_CONTAINS: if (argc == 2 && args[0].type == exprtk_VAL_STRING && args[1].type == exprtk_VAL_STRING) {
        result = exprtk_val_num(tstr_v_contains(args[0].data.string, args[1].data.string) ? 1.0 : 0.0);
    }
    break;
    case BI_STARTS_WITH: if (argc == 2 && args[0].type == exprtk_VAL_STRING && args[1].type == exprtk_VAL_STRING) {
        result = exprtk_val_num(tstr_v_starts_with(args[0].data.string, args[1].data.string) ? 1.0 : 0.0);
    }
    break;
    case BI_ENDS_WITH: if (argc == 2 && args[0].type == exprtk_VAL_STRING && args[1].type == exprtk_VAL_STRING) {
        result = exprtk_val_num(tstr_v_ends_with(args[0].data.string, args[1].data.string) ? 1.0 : 0.0);
    }
    break;
    case BI_INDEX_OF: if (argc == 2 && args[0].type == exprtk_VAL_STRING && args[1].type == exprtk_VAL_STRING) {
        size_t pos = tstr_v_find(args[0].data.string, args[1].data.string);
        result = exprtk_val_num(pos == TSTR_V_NPOS ? -1.0 : (double)pos);
    }
    break;
    case BI_SUBSTR: if ((argc == 2 || argc == 3) && args[0].type == exprtk_VAL_STRING) {
        size_t start = (size_t)args[1].data.number;
        size_t len = (argc == 3) ? (size_t)args[2].data.number : TSTR_V_NPOS;
        result = exprtk_val_str(tstr_v_sub(args[0].data.string, start, len));
    }
    break;
    case BI_REVERSE: if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        tstr_v s = args[0].data.string;
        char *buf = turbo_arena_alloc(arena, s.len);
        if (buf) {
            for (size_t i = 0; i < s.len; ++i) buf[i] = s.data[s.len - 1 - i];
            result = exprtk_val_str(tstr_v_from_buf(buf, s.len));
        }
    }
    break;
    case BI_ASSERT: if ((argc == 1 || argc == 2)) {
        double cond = (args[0].type == exprtk_VAL_NUMBER) ? args[0].data.number : (args[0].data.string.len > 0);
        if (fabs(cond) <= 1e-9) {
            if (env) {
                env->aborted = 1;
                if (argc == 2 && args[1].type == exprtk_VAL_STRING) {
                    printf("Assertion failed: %.*s\n", (int)args[1].data.string.len, args[1].data.string.data);
                } else {
                    printf("Assertion failed\n");
                }
            }
        }
        result = args[0];
    }
    break;
    case BI_REPLACE: if (argc == 3 && args[0].type == exprtk_VAL_STRING && args[1].type == exprtk_VAL_STRING && args[2].type == exprtk_VAL_STRING) {
        tstr_v s = args[0].data.string;
        tstr_v old_v = args[1].data.string;
        tstr_v new_v = args[2].data.string;
        if (old_v.len == 0) {
            result = args[0];
        } else {
            size_t count = 0;
            size_t curr = 0;
            while (curr <= s.len) {
                size_t p = tstr_v_find(tstr_v_sub(s, curr, TSTR_V_NPOS), old_v);
                if (p == TSTR_V_NPOS) break;
                count++;
                curr += p + old_v.len;
                if (old_v.len == 0) break; 
            }
            if (count == 0) {
                result = args[0];
            } else {
                size_t new_len = s.len + count * (new_v.len - old_v.len);
                char *buf = turbo_arena_alloc(arena, new_len);
                if (buf) {
                    char *dest = buf;
                    size_t last_src = 0;
                    while (last_src < s.len) {
                        size_t p = tstr_v_find(tstr_v_sub(s, last_src, TSTR_V_NPOS), old_v);
                        if (p == TSTR_V_NPOS) break;
                        memcpy(dest, s.data + last_src, p);
                        dest += p;
                        memcpy(dest, new_v.data, new_v.len);
                        dest += new_v.len;
                        last_src += p + old_v.len;
                    }
                    if (last_src < s.len) memcpy(dest, s.data + last_src, s.len - last_src);
                    result = exprtk_val_str(tstr_v_from_buf(buf, new_len));
                }
            }
        }
    }
    break;
    case BI_MEDIAN: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        result = exprtk_val_num(exprtk_median(args[0].data.vector.data, args[0].data.vector.size, arena));
    }
    break;
    case BI_PERCENTILE: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_NUMBER) {
        result = exprtk_val_num(exprtk_percentile(args[0].data.vector.data, args[0].data.vector.size, args[1].data.number, arena));
    }
    break;
    case BI_SKEWNESS: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        result = exprtk_val_num(exprtk_skewness(args[0].data.vector.data, args[0].data.vector.size));
    }
    break;
    case BI_KURTOSIS: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        result = exprtk_val_num(exprtk_kurtosis(args[0].data.vector.data, args[0].data.vector.size));
    }
    break;
    case BI_GEOMETRIC_MEAN: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        result = exprtk_val_num(exprtk_geometric_mean(args[0].data.vector.data, args[0].data.vector.size));
    }
    break;
    case BI_HARMONIC_MEAN: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        result = exprtk_val_num(exprtk_harmonic_mean(args[0].data.vector.data, args[0].data.vector.size));
    }
    break;
    case BI_ZSCORE: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n >= 2) {
            double sum = 0;
            for (size_t i = 0; i < n; ++i) sum += args[0].data.vector.data[i];
            double mean = sum / (double)n;
            double m2 = 0;
            for (size_t i = 0; i < n; ++i) {
                double d = args[0].data.vector.data[i] - mean;
                m2 += d * d;
            }
            double sd = sqrt(m2 / (double)(n - 1));
            if (sd > 1e-15) {
                double *res_data = ALLOC_DBL(arena, n);
                if (res_data) {
                    for (size_t i = 0; i < n; ++i) res_data[i] = (args[0].data.vector.data[i] - mean) / sd;
                    result = exprtk_val_vec(res_data, n);
                }
            }
        }
    }
    break;
    case BI_WMEAN: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n > 0 && args[1].data.vector.size >= n) {
            double sw = 0, swx = 0;
            for (size_t i = 0; i < n; ++i) { sw += args[1].data.vector.data[i]; swx += args[1].data.vector.data[i] * args[0].data.vector.data[i]; }
            if (fabs(sw) > 1e-15) result = exprtk_val_num(swx / sw);
        }
    }
    break;
    case BI_WVAR: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n >= 2 && args[1].data.vector.size >= n) {
            double sw = 0, swx = 0;
            for (size_t i = 0; i < n; ++i) { sw += args[1].data.vector.data[i]; swx += args[1].data.vector.data[i] * args[0].data.vector.data[i]; }
            if (fabs(sw) > 1e-15) {
                double wm = swx / sw;
                double num = 0;
                for (size_t i = 0; i < n; ++i) { double d = args[0].data.vector.data[i] - wm; num += args[1].data.vector.data[i] * d * d; }
                result = exprtk_val_num(num / sw);
            }
        }
    }
    break;
    case BI_EWMA: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_NUMBER) {
        size_t n = args[0].data.vector.size;
        double alpha = args[1].data.number;
        if (alpha > 1.0) alpha = 2.0 / (alpha + 1.0);
        if (n > 0) {
            double *res_data = ALLOC_DBL(arena, n);
            if (res_data) {
                res_data[0] = args[0].data.vector.data[0];
                for (size_t i = 1; i < n; ++i)
                    res_data[i] = alpha * args[0].data.vector.data[i] + (1.0 - alpha) * res_data[i - 1];
                result = exprtk_val_vec(res_data, n);
            }
        }
    }
    break;
    case BI_EWMVAR: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_NUMBER) {
        size_t n = args[0].data.vector.size;
        double alpha = args[1].data.number;
        if (alpha > 1.0) alpha = 2.0 / (alpha + 1.0);
        if (n >= 2) {
            double *res_data = ALLOC_DBL(arena, n);
            if (res_data) {
                double ema = args[0].data.vector.data[0];
                res_data[0] = 0;
                for (size_t i = 1; i < n; ++i) {
                    double diff = args[0].data.vector.data[i] - ema;
                    ema = alpha * args[0].data.vector.data[i] + (1.0 - alpha) * ema;
                    res_data[i] = (1.0 - alpha) * (res_data[i - 1] + alpha * diff * diff);
                }
                result = exprtk_val_vec(res_data, n);
            }
        }
    }
    break;
    case BI_COVARIANCE: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n >= 2 && args[1].data.vector.size >= n) {
            double sx = 0, sy = 0;
            for (size_t i = 0; i < n; ++i) {
                sx += args[0].data.vector.data[i];
                sy += args[1].data.vector.data[i];
            }
            double mx = sx / (double)n;
            double my = sy / (double)n;
            double cov = 0;
            for (size_t i = 0; i < n; ++i)
                cov += (args[0].data.vector.data[i] - mx) * (args[1].data.vector.data[i] - my);
            result = exprtk_val_num(cov / (double)(n - 1));
        }
    }
    break;
    case BI_CUMSUM: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n > 0) {
            double *res_data = ALLOC_DBL(arena, n);
            if (res_data) {
                res_data[0] = args[0].data.vector.data[0];
                for (size_t i = 1; i < n; ++i) res_data[i] = res_data[i - 1] + args[0].data.vector.data[i];
                result = exprtk_val_vec(res_data, n);
            }
        }
    }
    break;
    case BI_CUMPROD: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n > 0) {
            double *res_data = ALLOC_DBL(arena, n);
            if (res_data) {
                res_data[0] = args[0].data.vector.data[0];
                for (size_t i = 1; i < n; ++i) res_data[i] = res_data[i - 1] * args[0].data.vector.data[i];
                result = exprtk_val_vec(res_data, n);
            }
        }
    }
    break;
    case BI_RANK: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n > 0) {
            double *res_data = ALLOC_DBL(arena, n);
            rank_item_t *items = (rank_item_t*)malloc(n * sizeof(rank_item_t));
            if (res_data && items) {
                for (size_t i = 0; i < n; ++i) { items[i].idx = i; items[i].val = args[0].data.vector.data[i]; }
                qsort(items, n, sizeof(rank_item_t), compare_rank_items);
                size_t i = 0;
                while (i < n) {
                    size_t j = i + 1;
                    while (j < n && fabs(items[j].val - items[i].val) < 1e-15) ++j;
                    double avg_rank = (double)(i + j + 1) / 2.0;
                    for (size_t k = i; k < j; ++k) res_data[items[k].idx] = avg_rank;
                    i = j;
                }
                result = exprtk_val_vec(res_data, n);
            }
            if (items) free(items);
        }
    }
    break;
    case BI_HISTOGRAM: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_NUMBER) {
        size_t n = args[0].data.vector.size;
        size_t nbins = (size_t)args[1].data.number;
        if (n > 0 && nbins > 0) {
            double lo = args[0].data.vector.data[0], hi = lo;
            for (size_t i = 1; i < n; ++i) {
                if (args[0].data.vector.data[i] < lo) lo = args[0].data.vector.data[i];
                if (args[0].data.vector.data[i] > hi) hi = args[0].data.vector.data[i];
            }
            double *res_data = ALLOC_DBL(arena, nbins);
            if (res_data) {
                memset(res_data, 0, nbins * sizeof(double));
                double range = hi - lo;
                if (range < 1e-15) {
                    res_data[0] = (double)n;
                } else {
                    double bin_width = range / (double)nbins;
                    for (size_t i = 0; i < n; ++i) {
                        size_t b = (size_t)((args[0].data.vector.data[i] - lo) / bin_width);
                        if (b >= nbins) b = nbins - 1;
                        res_data[b] += 1.0;
                    }
                }
                result = exprtk_val_vec(res_data, nbins);
            }
        }
    }
    break;
    case BI_FIBONACCI: if (argc == 1) {
        result = exprtk_val_num(exprtk_fibonacci((int)args[0].data.number));
    }
    break;
    case BI_GCD: if (argc == 2) {
        result = exprtk_val_num((double)exprtk_gcd((long long)args[0].data.number, (long long)args[1].data.number));
    }
    break;
    case BI_NORMAL_RAND: if ((argc == 0 || argc == 2)) {
        double mu = (argc == 2) ? args[0].data.number : 0.0;
        double sigma = (argc == 2) ? args[1].data.number : 1.0;
        result = exprtk_val_num(exprtk_normal_rand(mu, sigma));
    }
    break;
    case BI_DET2: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size >= 4) result = exprtk_val_num(exprtk_det2(args[0].data.vector.data));
    }
    break;
    case BI_DET3: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size >= 9) result = exprtk_val_num(exprtk_det3(args[0].data.vector.data));
    }
    break;
    case BI_INV2: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size >= 4) {
            double *res_data = ALLOC_DBL(arena, 4);
            if (res_data && exprtk_inv2(args[0].data.vector.data, res_data)) result = exprtk_val_vec(res_data, 4);
        }
    }
    break;
    case BI_INV3: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size >= 9) {
            double *res_data = ALLOC_DBL(arena, 9);
            if (res_data && exprtk_inv3(args[0].data.vector.data, res_data)) result = exprtk_val_vec(res_data, 9);
        }
    }
    break;
    case BI_MATMUL: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t m = (size_t)args[2].data.number, k = (size_t)args[3].data.number, n = (size_t)args[4].data.number;
        if (args[0].data.vector.size >= m * k && args[1].data.vector.size >= k * n) {
            double *res_data = ALLOC_DBL(arena, m * n);
            if (res_data) { exprtk_matmul(args[0].data.vector.data, args[1].data.vector.data, m, k, n, res_data); result = exprtk_val_vec(res_data, m * n); }
        }
    }
    break;
    case BI_TRANSPOSE: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t rows = (size_t)args[1].data.number, cols = (size_t)args[2].data.number;
        if (args[0].data.vector.size >= rows * cols) {
            double *res_data = ALLOC_DBL(arena, rows * cols);
            if (res_data) { exprtk_transpose(args[0].data.vector.data, rows, cols, res_data); result = exprtk_val_vec(res_data, rows * cols); }
        }
    }
    break;
    case BI_EIG2: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size >= 4) {
            double *ev = ALLOC_DBL(arena, 2);
            if (ev) { exprtk_eig2(args[0].data.vector.data, ev); result = exprtk_val_vec(ev, 2); }
        }
    }
    break;
    case BI_EIG3: if (argc == 2 && args[1].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size >= 9 && args[1].data.vector.size >= 3)
            result = exprtk_val_num((double)exprtk_eig3(args[0].data.vector.data, args[1].data.vector.data));
    }
    break;
    case BI_TRACE2: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        if (args[0].data.vector.size >= 4) result = exprtk_val_num(exprtk_trace2(args[0].data.vector.data));
    }
    // Backtest
    break;
    case BI_BT_BACKTEST: if (argc == 6 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *eq = ALLOC_DBL(arena, n);
        double *tr = ALLOC_DBL(arena, n);
        if (eq && tr) {
            size_t nt = exprtk_bt_backtest(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, args[3].data.number, args[4].data.number, eq, tr);
            result = exprtk_val_vec(eq, n); // Normally returning equity curve
        }
    }
    break;
    case BI_BT_STATS: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        double *stats = ALLOC_DBL(arena, 14);
        if (stats) {
            exprtk_bt_stats(args[0].data.vector.data, args[1].data.vector.data, args[0].data.vector.size, (size_t)args[2].data.number, args[3].data.number, stats);
            result = exprtk_val_vec(stats, 14);
        }
    }
    break;
    case BI_BT_BACKTEST_EX: if (argc == 9 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR && args[4].type == exprtk_VAL_VECTOR && args[5].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *eq = ALLOC_DBL(arena, n);
        double *tr = ALLOC_DBL(arena, n);
        if (eq && tr) {
            exprtk_bt_backtest_ex(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, args[5].data.vector.data, n, args[6].data.number, args[7].data.number, args[8].data.vector.data, args[8].data.vector.size, eq, tr, arena);
            result = exprtk_val_vec(eq, n);
        }
    }
    break;
    case BI_BT_PORTFOLIO: if (argc == 6 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t na = (size_t)args[2].data.number, nb = (size_t)args[3].data.number;
        double *eq = ALLOC_DBL(arena, nb);
        double *w = ALLOC_DBL(arena, na * nb);
        if (eq && w) {
            exprtk_bt_portfolio(args[0].data.vector.data, args[1].data.vector.data, na, nb, args[4].data.number, args[5].data.number, eq, w, arena);
            result = exprtk_val_vec(eq, nb);
        }
    }
    // Portfolio
    break;
    case BI_PF_COV_MATRIX: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t na = (size_t)args[1].data.number, np = args[0].data.vector.size / na;
        double *cov = ALLOC_DBL(arena, na * na);
        if (cov) { exprtk_pf_cov_matrix(args[0].data.vector.data, na, np, cov, arena); result = exprtk_val_vec(cov, na * na); }
    }
    break;
    case BI_PF_MIN_VARIANCE: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = (size_t)sqrt((double)args[0].data.vector.size); double *w = ALLOC_DBL(arena, n);
        if (w) { exprtk_pf_min_variance(args[0].data.vector.data, n, w, arena); result = exprtk_val_vec(w, n); }
    }
    break;
    case BI_PF_MAX_SHARPE: if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double rf = (argc > 2) ? args[2].data.number : 0.0;
        double *w = ALLOC_DBL(arena, n);
        if (w) { exprtk_pf_max_sharpe(args[0].data.vector.data, args[1].data.vector.data, n, rf, w, arena); result = exprtk_val_vec(w, n); }
    }
    break;
    case BI_PF_MARKOWITZ: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *w = ALLOC_DBL(arena, n);
        if (w) { exprtk_pf_markowitz(args[0].data.vector.data, args[1].data.vector.data, n, args[2].data.number, w, arena); result = exprtk_val_vec(w, n); }
    }
    break;
    case BI_PF_RISK_PARITY: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = (size_t)sqrt((double)args[0].data.vector.size); double *w = ALLOC_DBL(arena, n);
        if (w) { exprtk_pf_risk_parity(args[0].data.vector.data, n, w, arena); result = exprtk_val_vec(w, n); }
    }
    // Risk
    break;
    case BI_VAR_HIST: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double v; if (exprtk_var_hist(args[0].data.vector.data, args[0].data.vector.size, args[1].data.number, &v, arena)) result = exprtk_val_num(v);
    }
    break;
    case BI_VAR_PARAM: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double v; if (exprtk_var_param(args[0].data.vector.data, args[0].data.vector.size, args[1].data.number, &v)) result = exprtk_val_num(v);
    }
    break;
    case BI_CVAR: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double v; if (exprtk_cvar(args[0].data.vector.data, args[0].data.vector.size, args[1].data.number, &v, arena)) result = exprtk_val_num(v);
    }
    break;
    case BI_KELLY: if (argc == 3) {
        result = exprtk_val_num(exprtk_kelly(args[0].data.number, args[1].data.number, args[2].data.number));
    }
    // TA-Lib Overlap
    break;
    case BI_TA_SMA: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_sma(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_EMA: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_ema(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_WMA: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_wma(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_DEMA: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_dema(args[0].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_TEMA: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_tema(args[0].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_KAMA: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_kama(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_T3: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_t3(args[0].data.vector.data, n, p, args[2].data.number, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_TRIMA: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_trima(args[0].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BBANDS: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *up = ALLOC_DBL(arena, n);
        double *mid = ALLOC_DBL(arena, n);
        double *low = ALLOC_DBL(arena, n);
        if (up && mid && low && exprtk_ta_bbands(args[0].data.vector.data, n, p, args[2].data.number, up, mid, low)) result = exprtk_val_vec(mid, n);
    }
    break;
    case BI_TA_MIDPOINT: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_midpoint(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_MIDPRICE: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[2].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_midprice(args[0].data.vector.data, args[1].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_SAR: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_sar(args[0].data.vector.data, args[1].data.vector.data, n, args[2].data.number, args[3].data.number, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_SAVGOL: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t w = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_savgol(args[0].data.vector.data, n, w, out)) result = exprtk_val_vec(out, n);
    }
    // TA-Lib Momentum
    break;
    case BI_TA_RSI: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_rsi(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_MACD: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t fp = (size_t)args[1].data.number, sp = (size_t)args[2].data.number, sip = (size_t)args[3].data.number;
        double *line = ALLOC_DBL(arena, n);
        double *sig = ALLOC_DBL(arena, n);
        double *hist = ALLOC_DBL(arena, n);
        if (line && sig && hist && exprtk_ta_macd(args[0].data.vector.data, n, fp, sp, sip, line, sig, hist, arena)) result = exprtk_val_vec(line, n);
    }
    break;
    case BI_TA_STOCH: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t kp = (size_t)args[3].data.number, dp = (size_t)args[4].data.number;
        double *ok = ALLOC_DBL(arena, n);
        double *od = ALLOC_DBL(arena, n);
        if (ok && od && exprtk_ta_stoch(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, kp, dp, ok, od, arena)) result = exprtk_val_vec(ok, n);
    }
    break;
    case BI_TA_STOCHRSI: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number, kp = (size_t)args[2].data.number, dp = (size_t)args[3].data.number;
        double *ok = ALLOC_DBL(arena, n);
        double *od = ALLOC_DBL(arena, n);
        if (ok && od && exprtk_ta_stochrsi(args[0].data.vector.data, n, p, kp, dp, ok, od, arena)) result = exprtk_val_vec(ok, n);
    }
    break;
    case BI_TA_WILLR: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[3].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_willr(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_CCI: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[3].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_cci(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_MOM: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_mom(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_ROC: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_roc(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_APO: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t fp = (size_t)args[1].data.number, sp = (size_t)args[2].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_apo(args[0].data.vector.data, n, fp, sp, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_PPO: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t fp = (size_t)args[1].data.number, sp = (size_t)args[2].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_ppo(args[0].data.vector.data, n, fp, sp, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_TRIX: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_trix(args[0].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_ULTOSC: if (argc == 6 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p1 = (size_t)args[3].data.number, p2 = (size_t)args[4].data.number, p3 = (size_t)args[5].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_ultosc(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, p1, p2, p3, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_AROON: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[2].data.number;
        double *up = ALLOC_DBL(arena, n);
        double *dn = ALLOC_DBL(arena, n);
        if (up && dn && exprtk_ta_aroon(args[0].data.vector.data, args[1].data.vector.data, n, p, up, dn)) result = exprtk_val_vec(up, n);
    }
    break;
    case BI_TA_AROONOSC: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[2].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_aroonosc(args[0].data.vector.data, args[1].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_CMO: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_cmo(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    // TA-Lib Volatility
    break;
    case BI_TA_TRANGE: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_trange(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_ATR: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[3].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_atr(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_NATR: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[3].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_natr(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    // TA-Lib Volume
    break;
    case BI_TA_OBV: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_obv(args[0].data.vector.data, args[1].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_AD: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_ad(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_ADOSC: if (argc == 6 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t fp = (size_t)args[4].data.number, sp = (size_t)args[5].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_adosc(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, fp, sp, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_MFI: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[4].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_mfi(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    // TA-Lib Trend
    break;
    case BI_TA_PLUS_DM: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[2].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_plus_dm(args[0].data.vector.data, args[1].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_MINUS_DM: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[2].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_minus_dm(args[0].data.vector.data, args[1].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_PLUS_DI: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[3].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_plus_di(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_MINUS_DI: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[3].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_minus_di(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_DX: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[3].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_dx(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_ADX: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[3].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_adx(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_ADXR: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[3].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_adxr(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, p, out, arena)) result = exprtk_val_vec(out, n);
    }
    // TA-Lib Statistics
    break;
    case BI_TA_STDDEV: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_stddev(args[0].data.vector.data, n, p, args[2].data.number, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_VAR: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_var(args[0].data.vector.data, n, p, args[2].data.number, out, arena)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_LINEARREG: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_linearreg(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_LINEARREG_SLOPE: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_linearreg_slope(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_LINEARREG_INTERCEPT: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_linearreg_intercept(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_LINEARREG_ANGLE: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_linearreg_angle(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_TSF: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_tsf(args[0].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BETA: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[2].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_beta(args[0].data.vector.data, args[1].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_CORREL: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; size_t p = (size_t)args[2].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_correl(args[0].data.vector.data, args[1].data.vector.data, n, p, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_FIXED_FRAC: if (argc == 3) {
        result = exprtk_val_num(exprtk_fixed_frac(args[0].data.number, args[1].data.number, args[2].data.number));
    }
    break;
    case BI_OPTIMAL_F: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        double f; exprtk_optimal_f(args[0].data.vector.data, args[0].data.vector.size, &f); result = exprtk_val_num(f);
    }
    break;
    case BI_DRAWDOWN: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *dd = ALLOC_DBL(arena, n);
        if (dd) { exprtk_drawdown(args[0].data.vector.data, n, dd); result = exprtk_val_vec(dd, n); }
    }
    break;
    case BI_DRAWDOWN_STATS: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        double *s = ALLOC_DBL(arena, 3);
        if (s) { exprtk_drawdown_stats(args[0].data.vector.data, args[0].data.vector.size, s); result = exprtk_val_vec(s, 3); }
    }
    // Signal
    break;
    case BI_CROSSOVER: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_crossover(args[0].data.vector.data, args[1].data.vector.data, n, out); result = exprtk_val_vec(out, n); }
    }
    break;
    case BI_CROSSUNDER: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_crossunder(args[0].data.vector.data, args[1].data.vector.data, n, out); result = exprtk_val_vec(out, n); }
    }
    break;
    case BI_SIGNAL_COMBINE: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t nb = args[0].data.vector.size / args[1].data.vector.size; double *out = ALLOC_DBL(arena, nb);
        if (out) { exprtk_signal_combine(args[0].data.vector.data, args[1].data.vector.data, nb, args[1].data.vector.size, out); result = exprtk_val_vec(out, nb); }
    }
    break;
    case BI_CANDLE_DOJI: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_candle_doji(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, args[4].data.number, out); result = exprtk_val_vec(out, n); }
    }
    break;
    case BI_CANDLE_HAMMER: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_candle_hammer(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, out); result = exprtk_val_vec(out, n); }
    }
    break;
    case BI_CANDLE_ENGULFING: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_candle_engulfing(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, out); result = exprtk_val_vec(out, n); }
    }
    break;
    case BI_CANDLE_MORNINGSTAR: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_candle_morningstar(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, out); result = exprtk_val_vec(out, n); }
    }
    // Timeseries
    break;
    case BI_TS_DIFF: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ts_diff(args[0].data.vector.data, n, (size_t)args[1].data.number, out, arena); result = exprtk_val_vec(out, n); }
    }
    break;
    case BI_TS_AUTOCORR: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t ml = (size_t)args[1].data.number; double *out = ALLOC_DBL(arena, ml + 1);
        if (out) { exprtk_ts_autocorr(args[0].data.vector.data, args[0].data.vector.size, ml, out); result = exprtk_val_vec(out, ml + 1); }
    }
    break;
    case BI_TS_PACF: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t ml = (size_t)args[1].data.number; double *out = ALLOC_DBL(arena, ml + 1);
        if (out) { exprtk_ts_pacf(args[0].data.vector.data, args[0].data.vector.size, ml, out, arena); result = exprtk_val_vec(out, ml + 1); }
    }
    break;
    case BI_TS_ADF: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double *out = ALLOC_DBL(arena, 2);
        if (out) { exprtk_ts_adf(args[0].data.vector.data, args[0].data.vector.size, (size_t)args[1].data.number, out, arena); result = exprtk_val_vec(out, 2); }
    }
    break;
    case BI_TS_GARCH: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ts_garch(args[0].data.vector.data, n, args[1].data.number, args[2].data.number, out); result = exprtk_val_vec(out, n); }
    }
    break;
    case BI_TS_HURST: if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        result = exprtk_val_num(exprtk_ts_hurst(args[0].data.vector.data, args[0].data.vector.size, NULL, arena));
    }
    // TA-Lib Price Transform
    break;
    case BI_TA_AVGPRICE: if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_avgprice(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_MEDPRICE: if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_medprice(args[0].data.vector.data, args[1].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_TYPPRICE: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_typprice(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_WCLPRICE: if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_wclprice(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    // TA-Lib Options
    break;
    case BI_TA_BSM_CALL: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_call(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BSM_PUT: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_put(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BSM_DELTA_CALL: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_delta_call(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BSM_DELTA_PUT: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_delta_put(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BSM_GAMMA: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_gamma(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BSM_THETA_CALL: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_theta_call(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BSM_THETA_PUT: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_theta_put(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BSM_VEGA: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_vega(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BSM_RHO_CALL: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_rho_call(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BSM_RHO_PUT: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_rho_put(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BSM_IV_CALL: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_iv_call(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_BSM_IV_PUT: if (argc == 5 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_bsm_iv_put(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, n, out)) result = exprtk_val_vec(out, n);
    }
    break;
    case BI_TA_OPT_BINOMIAL: if (argc == 7) {
        double opt; if (exprtk_ta_opt_binomial(args[0].data.number, args[1].data.number, args[2].data.number, args[3].data.number, args[4].data.number, (size_t)args[5].data.number, (int)args[6].data.number, &opt, arena)) result = exprtk_val_num(opt);
    }





    break;
    case BI_INTEGRATE: if ((argc == 3 || argc == 4)) {
        if (args[0].type == exprtk_VAL_STRING) {
            char f_name[256];
            size_t len = args[0].data.string.len;
            if (len > 255) len = 255;
            memcpy(f_name, args[0].data.string.data, len);
            f_name[len] = '\0';
            
            double a = args[1].data.number;
            double b = args[2].data.number;
            int n = (argc == 4) ? (int)args[3].data.number : 1000;
            if (n <= 0) n = 1000;
            if (n % 2 != 0) n++;
            
            double h = (b - a) / n;
            double sum = 0;
            
            exprtk_value_t vx_a = exprtk_val_num(a);
            exprtk_value_t vx_b = exprtk_val_num(b);
            exprtk_value_t v_a = exprtk_call_internal(f_name, 1, &vx_a, env, arena);
            exprtk_value_t v_b = exprtk_call_internal(f_name, 1, &vx_b, env, arena);
            sum = v_a.data.number + v_b.data.number;
            
            for (int i = 1; i < n; ++i) {
                if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) break;
                double x = a + i * h;
                exprtk_value_t vx_val = exprtk_val_num(x);
                exprtk_value_t v_x = exprtk_call_internal(f_name, 1, &vx_val, env, arena);
                sum += (i % 2 == 0 ? 2.0 : 4.0) * v_x.data.number;
            }
            result = exprtk_val_num((h / 3.0) * sum);
        }
    }
    break;
    case BI_DERIVATIVE: if ((argc == 2 || argc == 3)) {
        if (args[0].type == exprtk_VAL_STRING) {
            char f_name[256];
            size_t len = args[0].data.string.len;
            if (len > 255) len = 255;
            memcpy(f_name, args[0].data.string.data, len);
            f_name[len] = '\0';
            
            double x = args[1].data.number;
            double h = (argc == 3) ? args[2].data.number : 1e-6;
            
            exprtk_value_t v1_arg = exprtk_val_num(x + h);
            exprtk_value_t v2_arg = exprtk_val_num(x - h);
            exprtk_value_t v1 = exprtk_call_internal(f_name, 1, &v1_arg, env, arena);
            exprtk_value_t v2 = exprtk_call_internal(f_name, 1, &v2_arg, env, arena);
            
            result = exprtk_val_num((v1.data.number - v2.data.number) / (2.0 * h));
        }
    }
    // 3. File System Functions
    break;
    case BI_READ_FILE: if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        turbo_fs_buf_t buf = {0};
        char *path = turbo_arena_alloc(arena, args[0].data.string.len + 1);
        if (path) {
            memcpy(path, args[0].data.string.data, args[0].data.string.len);
            path[args[0].data.string.len] = '\0';
            if (turbo_fs_read_file(path, &buf) == 0) {
                char *data = turbo_arena_alloc(arena, buf.len);
                if (data) {
                    memcpy(data, buf.base, buf.len);
                    result = exprtk_val_str(tstr_v_from_buf(data, buf.len));
                }
                turbo_fs_buf_free(&buf);
            }
        }
    }
    break;
    case BI_WRITE_FILE: if (argc == 2 && args[0].type == exprtk_VAL_STRING && args[1].type == exprtk_VAL_STRING) {
        char *path = turbo_arena_alloc(arena, args[0].data.string.len + 1);
        if (path) {
            memcpy(path, args[0].data.string.data, args[0].data.string.len);
            path[args[0].data.string.len] = '\0';
            turbo_fs_buf_t buf;
            buf.base = (char*)args[1].data.string.data;
            buf.len = args[1].data.string.len;
            result = exprtk_val_num((double)turbo_fs_write_file(path, &buf));
        }
    }
    break;
    case BI_FILE_EXISTS: if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        char *path = turbo_arena_alloc(arena, args[0].data.string.len + 1);
        if (path) {
            memcpy(path, args[0].data.string.data, args[0].data.string.len);
            path[args[0].data.string.len] = '\0';
            turbo_fs_stat_t st;
            result = exprtk_val_num(turbo_fs_stat(path, &st) == 0 ? 1.0 : 0.0);
        }
    }
    // 4. Date/Time Functions
    break;
    case BI_NOW: {
        double t = (double)time(NULL);
        result = exprtk_val_num(t);
    }
    break;
    case BI_DATE: if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        datetime_t dt;
        // Make a null-terminated string for parsing
        char *ds = turbo_arena_alloc(arena, args[0].data.string.len + 1);
        if (ds) {
            memcpy(ds, args[0].data.string.data, args[0].data.string.len);
            ds[args[0].data.string.len] = '\0';
            if (datetime_parse(ds, args[0].data.string.len, &dt) == 0) {
                result = exprtk_val_num((double)datetime_to_time(&dt));
            }
        }
    }
    break;
    case BI_FORMAT_DATE: if ((argc == 1 || argc == 2)) {
        time_t t = (time_t)args[0].data.number;
        char buf[128];
        if (argc == 2 && args[1].type == exprtk_VAL_STRING) {
             // custom format not fully supported by datetime_parser.h yet, use standard strftime
             char *fmt = turbo_arena_alloc(arena, args[1].data.string.len + 1);
             if (fmt) {
                memcpy(fmt, args[1].data.string.data, args[1].data.string.len);
                fmt[args[1].data.string.len] = '\0';
                struct tm *tm_info = localtime(&t);
                if (tm_info && strftime(buf, sizeof(buf), fmt, tm_info) > 0) {
                    size_t len = strlen(buf);
                    char *res_buf = turbo_arena_alloc(arena, len);
                    if (res_buf) {
                        memcpy(res_buf, buf, len);
                        result = exprtk_val_str(tstr_v_from_buf(res_buf, len));
                    }
                }
             }
        } else {
            // Default to RFC 822
            if (datetime_format_rfc822(t, buf, sizeof(buf)) > 0) {
                size_t len = strlen(buf);
                char *res_buf = turbo_arena_alloc(arena, len);
                if (res_buf) {
                    memcpy(res_buf, buf, len);
                    result = exprtk_val_str(tstr_v_from_buf(res_buf, len));
                }
            }
        }
    }

    } /* end switch */

    return result;
}

