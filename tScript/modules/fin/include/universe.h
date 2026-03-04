/**
 * @file universe.h
 * @brief Universe management: asset metadata, price adjustments, and per-date
 *        active-asset tracking for point-in-time correct backtesting.
 *
 * The universe layer ensures:
 *  - Assets only appear after their IPO / first trade date.
 *  - Assets are automatically retired on their delist date.
 *  - Historical prices are adjusted for splits and dividends.
 *  - Cross-sectional operations (rank, top-N, filter) only touch active assets.
 */
#ifndef UNIVERSE_H
#define UNIVERSE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "exprtk_types.h"   /* turbo_pool_t (via exprtk_types -> turbo_buff) */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Asset type codes */
#define UNIVERSE_ASSET_EQUITY   0
#define UNIVERSE_ASSET_ETF      1
#define UNIVERSE_ASSET_FUTURE   2
#define UNIVERSE_ASSET_CRYPTO   3
#define UNIVERSE_ASSET_FX       4
#define UNIVERSE_ASSET_BOND     5

/** Adjustment event types */
#define UNIVERSE_ADJ_SPLIT      0   /**< Factor = new/old ratio (e.g., 4.0 for 4:1) */
#define UNIVERSE_ADJ_DIVIDEND   1   /**< Factor = cash dividend per share            */
#define UNIVERSE_ADJ_DELIST     2   /**< Asset permanently removed                   */
#define UNIVERSE_ADJ_NAME       3   /**< Ticker rename (no price effect)             */

/** date sentinel: 0 means "still active / no end date" */
#define UNIVERSE_DATE_NONE  0.0

/* =========================================================================
 * Data Structures
 * ========================================================================= */

/**
 * @brief Metadata for a single tradable asset.
 *
 * Dates are stored as epoch days (days since 1970-01-01) as doubles,
 * consistent with exprtk_finance.c date conventions.
 */
typedef struct {
    uint32_t id;                /**< Stable numeric ID (primary key)           */
    char     ticker[16];        /**< e.g. "AAPL", "600519.SH", "BTC-USDT"     */
    char     exchange[8];       /**< e.g. "NYSE", "SSE", "BINANCE"             */
    uint8_t  asset_type;        /**< UNIVERSE_ASSET_* constant                 */
    double   start_date;        /**< First tradable date (epoch days)          */
    double   end_date;          /**< Last tradable date; 0 = still active      */
    double   lot_size;          /**< Min tradable quantity                      */
    double   tick_size;         /**< Min price increment                       */
    double   margin_req;        /**< Margin ratio (futures/crypto), 0 = full   */
} universe_asset_t;

/**
 * @brief A single price-adjustment event for an asset.
 *
 * Adjustments are applied backward (i.e., historical prices are scaled)
 * so that the adjusted series is continuous from the perspective of today.
 */
typedef struct {
    uint32_t asset_id;          /**< References universe_asset_t.id           */
    double   date;              /**< Date of the event (epoch days)            */
    uint8_t  type;              /**< UNIVERSE_ADJ_* constant                   */
    double   factor;            /**< Split ratio or dividend amount            */
    double   cum_adj;           /**< Cumulative backward adjustment factor     */
} universe_adj_t;

/**
 * @brief The runtime universe state.
 *
 * Holds all asset metadata and adjustments, plus the active-asset mask
 * for the current simulation date.  All memory is owned by the caller's
 * arena; free it by releasing that arena.
 */
typedef struct {
    /* Asset registry */
    universe_asset_t *assets;
    size_t            num_assets;
    size_t            assets_cap;

    /* Adjustment events (sorted by asset_id, then date) */
    universe_adj_t   *adjustments;
    size_t            num_adj;
    size_t            adj_cap;

    /* Per-date runtime state (updated by universe_advance) */
    uint8_t          *active_mask;      /**< [num_assets]: 1 = tradable today  */
    double           *cum_adj_cache;    /**< [num_assets]: current adj factor  */
    double            current_date;     /**< Last date passed to advance()     */

    /* Delistings detected in the most recent advance() call */
    uint32_t         *delisted_today;
    size_t            num_delisted_today;

    turbo_pool_t    *arena;
} universe_t;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * @brief Create a new empty universe.
 * @param arena  Lifetime arena; all universe memory is allocated here.
 * @return Pointer to universe, or NULL on OOM.
 */
universe_t *universe_create(turbo_pool_t *arena);

/**
 * @brief Free a universe (releases heap-allocated arrays, not the arena).
 */
void universe_free(universe_t *u);

/* =========================================================================
 * Building the Universe
 * ========================================================================= */

/**
 * @brief Add a single asset to the registry.
 * @return 0 on success, -1 on OOM.
 */
int universe_add_asset(universe_t *u, const universe_asset_t *asset);

/**
 * @brief Add a single adjustment event.
 * After all events are added, call universe_sort_adjustments() once.
 * @return 0 on success, -1 on OOM.
 */
int universe_add_adjustment(universe_t *u, const universe_adj_t *adj);

/**
 * @brief Sort adjustments and recompute cumulative factors.
 * Must be called once after all add_adjustment() calls and before
 * the first universe_advance().
 */
void universe_finalize(universe_t *u);

/* =========================================================================
 * Per-Bar Simulation
 * ========================================================================= */

/**
 * @brief Advance the universe clock to a new date.
 *
 * Updates active_mask, applies new adjustments to cum_adj_cache, and
 * populates delisted_today[] with any assets whose end_date == date.
 *
 * @param date  New simulation date (epoch days).
 */
void universe_advance(universe_t *u, double date);

/**
 * @brief Check if an asset is tradable on the current date.
 */
bool universe_is_active(const universe_t *u, uint32_t asset_id);

/**
 * @brief Return the number of currently active assets.
 */
size_t universe_active_count(const universe_t *u);

/**
 * @brief Fill out[] with IDs of all currently active assets.
 * @return Number of IDs written.
 */
size_t universe_active_ids(const universe_t *u, uint32_t *out, size_t max);

/**
 * @brief Return the cumulative backward price-adjustment factor
 *        for the given asset at the current universe date.
 *        Multiply raw historical prices by this factor to get
 *        the adjusted (continuous) price.
 */
double universe_adj_factor(const universe_t *u, uint32_t asset_id);

/**
 * @brief Convenience: apply the current adjustment factor to a raw price.
 */
double universe_adjust_price(const universe_t *u, uint32_t asset_id, double raw_price);

/**
 * @brief Fill out[] with asset IDs that were delisted on the last advance().
 * These positions must be force-closed before the next bar is processed.
 * @return Number of delisted IDs written.
 */
size_t universe_delisted_today(const universe_t *u, uint32_t *out, size_t max);

/* =========================================================================
 * Cross-Sectional Operations  (operate only on active assets)
 * ========================================================================= */

/**
 * @brief Compute intra-universe percentile rank for each active asset.
 *
 * @param values  Array of length num_assets; inactive slots are ignored.
 * @param out     Array of length num_assets; inactive slots are set to 0.
 *                Active slots receive a value in [0, 1] where 1 = highest.
 */
void universe_rank(const universe_t *u, const double *values, size_t n, double *out);

/**
 * @brief Return the IDs of the top-k active assets by value (descending).
 * @return Number of IDs written (≤ k).
 */
size_t universe_top_n(const universe_t *u, const double *values, size_t n,
                      size_t k, uint32_t *out_ids);

/**
 * @brief Build a boolean mask: mask[i] = 1 if active AND values[i] > threshold.
 * @return Number of assets that passed the filter.
 */
size_t universe_filter_gt(const universe_t *u, const double *values, size_t n,
                          double threshold, uint8_t *mask);

/* =========================================================================
 * Vectorized / SIMD-accelerated Cross-Sectional Helpers
 * ========================================================================= */

/**
 * @brief Bulk-adjust an array of raw prices using SIMD element-wise multiply.
 *        adjusted[i] = raw[i] * cum_adj_cache[i]
 */
void universe_adjust_prices(const universe_t *u, const double *raw, double *adjusted, size_t n);

/**
 * @brief Cross-sectional z-score normalization of active assets using SIMD.
 *        Inactive slots are set to 0.
 */
void universe_zscore(const universe_t *u, const double *values, size_t n, double *out);

/**
 * @brief Clamp all values to [lo, hi] using SIMD element-wise clip.
 */
void universe_clip(const universe_t *u, const double *values, size_t n,
                   double lo, double hi, double *out);

/**
 * @brief Sum values of active assets using SIMD reduction.
 */
double universe_cross_sum(const universe_t *u, const double *values, size_t n);

/**
 * @brief De-mean active asset values (subtract cross-sectional mean) using SIMD.
 *        Inactive slots are set to 0.
 */
void universe_demean(const universe_t *u, const double *values, size_t n, double *out);

/**
 * @brief Find the internal array index for a given asset ID.
 * @return Index, or SIZE_MAX if not found.
 */
size_t universe_find_asset(const universe_t *u, uint32_t asset_id);

/**
 * @brief Find an asset by its ticker string.
 * @return Pointer to asset, or NULL if not found.
 */
const universe_asset_t *universe_find_by_ticker(const universe_t *u, const char *ticker);

#ifdef __cplusplus
}
#endif

#endif /* UNIVERSE_H */
