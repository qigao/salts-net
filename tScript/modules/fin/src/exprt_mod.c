/* =========================================================================
 * Module table and factory
 * ========================================================================= */
#include "exprtk.h"
#include "exprtk_universe.h"
#include "exprtk_module.h"
#include "exprtk_mod_strategy.h"
#include "fin.h"
#include "simd_helpers.h"
#include "strategy_optimizer.h"
#include <math.h> 
static const exprtk_func_entry_t strategy_entries[] = {
    /* --- Signal setters ------------------------------------------------- */
    {"buy", fn_buy},
    {"sell", fn_sell},
    {"flat", fn_flat},
    {"set_sl", fn_set_sl},
    {"set_tp", fn_set_tp},

    /* --- Position queries ----------------------------------------------- */
    {"pos", fn_pos},
    {"entry_px", fn_entry_px},
    {"is_long", fn_is_long},
    {"is_short", fn_is_short},
    {"is_flat", fn_is_flat},
    {"unrealized_pnl", fn_unrealized_pnl},

    /* --- Universe cross-section ----------------------------------------- */
    {"rank_pct", fn_rank_pct},
    {"vec_rank", fn_vec_rank},
    {"vec_top", fn_vec_top},
    {"vec_filter_gt", fn_vec_filter_gt},
    {"vec_filter_lt", fn_vec_filter_lt},
    {"vec_where", fn_vec_where},
    {"vec_sort_idx", fn_vec_sort_idx},
    {"vec_shift", fn_vec_shift},
    {"vec_corr", fn_vec_corr},

    /* --- Risk sizing ---------------------------------------------------- */
    {"kelly_size", fn_kelly_size},
    {"risk_size", fn_risk_size},
    {"atr_sl", fn_atr_sl},

    /* --- Trade log queries --------------------------------------------- */
    {"num_trades", fn_num_trades},
    {"cum_pnl", fn_cum_pnl},
    {"win_rate", fn_win_rate},

    /* --- Bar helpers ---------------------------------------------------- */
    {"last", fn_last},
    {"prev", fn_prev},
    {"vec_sum", fn_vec_sum},
    {"vec_mean", fn_vec_mean},
    {"vec_std", fn_vec_std},
    {"vec_max", fn_vec_max},
    {"vec_min", fn_vec_min},
    {"vec_roll_max", fn_vec_roll_max},
    {"vec_roll_min", fn_vec_roll_min},
    {"vec_vmax", fn_vec_vmax},
    {"vec_vmin", fn_vec_vmin},
    {"vec_fill", fn_vec_fill},
    {"vec_at", fn_vec_at},
    {"vec_any", fn_vec_any},
    {"vec_all", fn_vec_all},
    {"vec_cross", fn_vec_cross},
    {"vec_resample", fn_vec_resample},

    /* Performance Metrics */
    {"sharpe", fn_sharpe},
    {"sortino", fn_sortino},
    {"calmar", fn_calmar},
    {"profit_factor", fn_profit_factor},
    {"expectancy", fn_expectancy},
    {"payoff_ratio", fn_payoff_ratio},
    {"max_dd_duration", fn_max_dd_duration},
    {"ulcer_index", fn_ulcer_index},
    {"information_ratio", fn_information_ratio},
    {"treynor", fn_treynor},

    /* Walk-Forward & Multi-Timeframe */
    {"resample_ohlcv", fn_resample_ohlcv},
    {"walk_forward", fn_walk_forward},

    /* Graph Algorithms (Network Analysis) */
    {"bellman_ford", exprtk_graph_bellman_ford},
    {"has_negative_cycle", exprtk_graph_has_negative_cycle},
    {"extract_path", exprtk_graph_extract_path},
    {"detect_arbitrage", exprtk_graph_detect_arbitrage},

    /* --- Universe (Real) ------------------------------------------------ */
    {"univ_is_active", fn_universe_is_active},
    {"univ_active_count", fn_universe_active_count},
    {"univ_adj_factor", fn_universe_adj_factor},
    {"univ_adjust_price", fn_universe_adjust_price},
    {"univ_adjust_prices", fn_universe_adjust_prices},
    {"univ_rank", fn_universe_rank},
    {"univ_top_n", fn_universe_top_n},
    {"univ_filter_gt", fn_universe_filter_gt},
    {"univ_zscore", fn_universe_zscore},
    {"univ_clip", fn_universe_clip},
    {"univ_cross_sum", fn_universe_cross_sum},
    {"univ_demean", fn_universe_demean},

};

static const exprtk_module_t strategy_module = {
    "strategy", strategy_entries, sizeof(strategy_entries) / sizeof(strategy_entries[0])};

const exprtk_module_t *exprtk_module_strategy(void) { return &strategy_module; }

