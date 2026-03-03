/**
 * @file universe.c
 * @brief Universe management implementation.
 *
 * Key design choices:
 *  - Assets are stored in a flat array and looked up by their index.
 *    The `universe_find_asset` helper maps a uint32_t id to that index.
 *  - Adjustments are sorted by (asset_id, date) and accessed via binary
 *    search to find the current cumulative factor in O(log N).
 *  - The active_mask and cum_adj_cache are updated once per bar by
 *    universe_advance(), so per-asset checks during strategy execution
 *    are O(1) array lookups.
 *  - Cross-sectional operations (rank, top_n, filter) use the active_mask
 *    to skip inactive assets and operate only on relevant data.
 */

#include "universe.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

#define INITIAL_ASSET_CAP  64
#define INITIAL_ADJ_CAP    256

static int adj_compare(const void *a, const void *b) {
    const universe_adj_t *x = (const universe_adj_t *)a;
    const universe_adj_t *y = (const universe_adj_t *)b;
    if (x->asset_id != y->asset_id)
        return (x->asset_id < y->asset_id) ? -1 : 1;
    if (x->date < y->date) return -1;
    if (x->date > y->date) return  1;
    return 0;
}

/* Binary search: find index of first adjustment for asset_id with date > d */
static size_t adj_upper_bound(const universe_adj_t *adj, size_t n,
                               uint32_t asset_id, double d) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (adj[mid].asset_id < asset_id ||
            (adj[mid].asset_id == asset_id && adj[mid].date <= d))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Find first adjustment for asset_id with date >= d */
static size_t adj_lower_bound(const universe_adj_t *adj, size_t n,
                               uint32_t asset_id, double d) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (adj[mid].asset_id < asset_id ||
            (adj[mid].asset_id == asset_id && adj[mid].date < d))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

universe_t *universe_create(turbo_arena_t *arena) {
    universe_t *u = (universe_t *)turbo_arena_alloc(arena, sizeof(universe_t));
    if (!u) return NULL;
    memset(u, 0, sizeof(*u));
    u->arena = arena;

    u->assets = (universe_asset_t *)malloc(INITIAL_ASSET_CAP * sizeof(universe_asset_t));
    if (!u->assets) return NULL;
    u->assets_cap = INITIAL_ASSET_CAP;

    u->adjustments = (universe_adj_t *)malloc(INITIAL_ADJ_CAP * sizeof(universe_adj_t));
    if (!u->adjustments) { free(u->assets); return NULL; }
    u->adj_cap = INITIAL_ADJ_CAP;

    /* active_mask, cum_adj_cache, and delisted_today are allocated in
     * universe_finalize() once we know num_assets. */

    return u;
}

void universe_free(universe_t *u) {
    if (!u) return;
    free(u->assets);
    free(u->adjustments);
    /* active_mask, cum_adj_cache, delisted_today are arena-allocated;
     * they are freed together with the arena by the caller. */
    u->assets = NULL;
    u->adjustments = NULL;
}

/* =========================================================================
 * Building the Universe
 * ========================================================================= */

int universe_add_asset(universe_t *u, const universe_asset_t *asset) {
    if (u->num_assets >= u->assets_cap) {
        size_t new_cap = u->assets_cap * 2;
        universe_asset_t *tmp = (universe_asset_t *)realloc(
            u->assets, new_cap * sizeof(universe_asset_t));
        if (!tmp) return -1;
        u->assets = tmp;
        u->assets_cap = new_cap;
    }
    u->assets[u->num_assets++] = *asset;
    return 0;
}

int universe_add_adjustment(universe_t *u, const universe_adj_t *adj) {
    if (u->num_adj >= u->adj_cap) {
        size_t new_cap = u->adj_cap * 2;
        universe_adj_t *tmp = (universe_adj_t *)realloc(
            u->adjustments, new_cap * sizeof(universe_adj_t));
        if (!tmp) return -1;
        u->adjustments = tmp;
        u->adj_cap = new_cap;
    }
    u->adjustments[u->num_adj++] = *adj;
    return 0;
}

void universe_finalize(universe_t *u) {
    if (!u) return;

    /* 1. Sort adjustments by (asset_id, date) */
    if (u->num_adj > 1)
        qsort(u->adjustments, u->num_adj, sizeof(universe_adj_t), adj_compare);

    /* 2. Compute cumulative backward adjustment factors.
     *
     *    For price data to be continuous, we multiply every historical
     *    price *before* an adjustment event by the inverse of the
     *    adjustment factor.  The cum_adj stored here is the factor the
     *    strategy gets: adjusted_price = raw_price * cum_adj.
     *
     *    We iterate forward in time per-asset and accumulate the product
     *    of all factors up to and including each event.
     */
    for (size_t i = 0; i < u->num_adj; ) {
        uint32_t aid  = u->adjustments[i].asset_id;
        double   cum  = 1.0;

        /* Walk all events for this asset in date order */
        size_t j = i;
        while (j < u->num_adj && u->adjustments[j].asset_id == aid) {
            if (u->adjustments[j].type == UNIVERSE_ADJ_SPLIT) {
                /* factor = new/old (e.g., 4.0 for 4:1 split).
                 * Backward-adjust: old prices multiplied by 1/factor.
                 * cum_adj represents "multiply raw by this to get adjusted". */
                cum /= u->adjustments[j].factor;
            }
            /* Dividend adjustments affect the price similarly.
             * If prev_close = P and dividend = D, adj prev_close = P - D.
             * For vector store, we record relative factor: (P-D)/P.
             * Since we don't know P here, we mark it for per-bar application.
             * Dividend events set cum_adj = 0 as a sentinel; handled in advance(). */
            u->adjustments[j].cum_adj = cum;
            j++;
        }
        i = j;
    }

    /* 3. Allocate per-date runtime arrays */
    size_t n = u->num_assets;
    if (n == 0) return;

    u->active_mask    = (uint8_t *)turbo_arena_alloc(u->arena, n * sizeof(uint8_t));
    u->cum_adj_cache  = (double  *)turbo_arena_alloc(u->arena, n * sizeof(double));
    u->delisted_today = (uint32_t *)turbo_arena_alloc(u->arena, n * sizeof(uint32_t));

    if (u->active_mask)   memset(u->active_mask,   0, n * sizeof(uint8_t));
    if (u->cum_adj_cache) {
        for (size_t i = 0; i < n; i++) u->cum_adj_cache[i] = 1.0;
    }

    u->current_date = 0.0;
}

/* =========================================================================
 * Per-Bar Simulation
 * ========================================================================= */

void universe_advance(universe_t *u, double date) {
    if (!u || u->num_assets == 0 || !u->active_mask) return;

    u->num_delisted_today = 0;
    double prev_date = u->current_date;
    u->current_date  = date;

    for (size_t i = 0; i < u->num_assets; i++) {
        const universe_asset_t *a = &u->assets[i];

        /* Check if asset is in its trading window */
        int now_active = (a->start_date <= date) &&
                         (a->end_date == UNIVERSE_DATE_NONE || a->end_date > date);

        /* Detect delistings: end_date fell within the (prev_date, date] window.
         * This fires on the LAST active bar (when the asset is still being
         * processed for the final time), so the strategy can close positions.
         * We fire independently of now_active so it can't be missed. */
        if (a->end_date > 0.0 && a->end_date > prev_date && a->end_date <= date) {
            if (u->delisted_today)
                u->delisted_today[u->num_delisted_today++] = a->id;
        }

        u->active_mask[i] = now_active ? 1 : 0;

        /* Update cum_adj_cache: find the latest adjustment event for this
         * asset with date ≤ current_date */
        if (u->adjustments && u->num_adj > 0) {
            /* Find last event with event.date <= date */
            size_t hi = adj_upper_bound(u->adjustments, u->num_adj, a->id, date);
            /* hi is one past the last valid event. Step back. */
            if (hi > 0) {
                size_t lo = adj_lower_bound(u->adjustments, u->num_adj, a->id, -DBL_MAX);
                /* hi-1 is the most recent event for this asset with date <= date */
                if (hi > lo && u->adjustments[hi - 1].asset_id == a->id) {
                    u->cum_adj_cache[i] = u->adjustments[hi - 1].cum_adj;
                } else {
                    u->cum_adj_cache[i] = 1.0;
                }
            } else {
                u->cum_adj_cache[i] = 1.0;
            }
        }
    }
}

bool universe_is_active(const universe_t *u, uint32_t asset_id) {
    size_t idx = universe_find_asset(u, asset_id);
    if (idx == SIZE_MAX) return false;
    return u->active_mask && u->active_mask[idx] != 0;
}

size_t universe_active_count(const universe_t *u) {
    if (!u || !u->active_mask) return 0;
    size_t count = 0;
    for (size_t i = 0; i < u->num_assets; i++)
        if (u->active_mask[i]) count++;
    return count;
}

size_t universe_active_ids(const universe_t *u, uint32_t *out, size_t max) {
    if (!u || !u->active_mask || !out) return 0;
    size_t count = 0;
    for (size_t i = 0; i < u->num_assets && count < max; i++)
        if (u->active_mask[i]) out[count++] = u->assets[i].id;
    return count;
}

double universe_adj_factor(const universe_t *u, uint32_t asset_id) {
    size_t idx = universe_find_asset(u, asset_id);
    if (idx == SIZE_MAX || !u->cum_adj_cache) return 1.0;
    return u->cum_adj_cache[idx];
}

double universe_adjust_price(const universe_t *u, uint32_t asset_id, double raw_price) {
    return raw_price * universe_adj_factor(u, asset_id);
}

size_t universe_delisted_today(const universe_t *u, uint32_t *out, size_t max) {
    if (!u || !u->delisted_today || !out) return 0;
    size_t n = u->num_delisted_today < max ? u->num_delisted_today : max;
    memcpy(out, u->delisted_today, n * sizeof(uint32_t));
    return n;
}

/* =========================================================================
 * Cross-Sectional Operations
 * ========================================================================= */

/* Internal: count active assets */
static size_t count_active(const universe_t *u) {
    size_t c = 0;
    for (size_t i = 0; i < u->num_assets; i++)
        if (u->active_mask[i]) c++;
    return c;
}

void universe_rank(const universe_t *u, const double *values, size_t n, double *out) {
    if (!u || !values || !out || n == 0) return;
    size_t na = u->num_assets < n ? u->num_assets : n;

    /* Zero out inactive */
    for (size_t i = 0; i < na; i++)
        out[i] = (!u->active_mask || u->active_mask[i]) ? values[i] : 0.0;

    /* For each active asset, compute its rank among active assets */
    size_t active_count = count_active(u);
    if (active_count == 0) return;

    for (size_t i = 0; i < na; i++) {
        if (!u->active_mask[i]) { out[i] = 0.0; continue; }
        size_t rank = 0;
        for (size_t j = 0; j < na; j++) {
            if (u->active_mask[j] && values[j] < values[i]) rank++;
        }
        out[i] = (double)rank / (double)(active_count - 1 > 0 ? active_count - 1 : 1);
    }
}

/* Pair for sorting in top_n */
typedef struct { size_t idx; double val; } rank_pair_t;
static int rp_cmp_desc(const void *a, const void *b) {
    double va = ((const rank_pair_t *)a)->val;
    double vb = ((const rank_pair_t *)b)->val;
    return (va > vb) ? -1 : (va < vb) ? 1 : 0;
}

size_t universe_top_n(const universe_t *u, const double *values, size_t n,
                      size_t k, uint32_t *out_ids) {
    if (!u || !values || !out_ids || n == 0 || k == 0) return 0;
    size_t na = u->num_assets < n ? u->num_assets : n;

    /* Collect active (index, value) pairs */
    rank_pair_t *tmp = (rank_pair_t *)malloc(na * sizeof(rank_pair_t));
    if (!tmp) return 0;
    size_t cnt = 0;
    for (size_t i = 0; i < na; i++) {
        if (u->active_mask && u->active_mask[i])
            tmp[cnt++] = (rank_pair_t){ i, values[i] };
    }

    qsort(tmp, cnt, sizeof(rank_pair_t), rp_cmp_desc);

    size_t written = cnt < k ? cnt : k;
    for (size_t i = 0; i < written; i++)
        out_ids[i] = u->assets[tmp[i].idx].id;

    free(tmp);
    return written;
}

size_t universe_filter_gt(const universe_t *u, const double *values, size_t n,
                          double threshold, uint8_t *mask) {
    if (!u || !values || !mask || n == 0) return 0;
    size_t na = u->num_assets < n ? u->num_assets : n;
    size_t count = 0;
    for (size_t i = 0; i < na; i++) {
        mask[i] = (u->active_mask && u->active_mask[i] && values[i] > threshold) ? 1 : 0;
        if (mask[i]) count++;
    }
    return count;
}

/* =========================================================================
 * Lookup helpers
 * ========================================================================= */

size_t universe_find_asset(const universe_t *u, uint32_t asset_id) {
    if (!u) return SIZE_MAX;
    /* Linear scan — acceptable for typical universe sizes (< 10k assets).
     * If needed, a hash map can be added as a future optimization. */
    for (size_t i = 0; i < u->num_assets; i++)
        if (u->assets[i].id == asset_id) return i;
    return SIZE_MAX;
}

const universe_asset_t *universe_find_by_ticker(const universe_t *u, const char *ticker) {
    if (!u || !ticker) return NULL;
    for (size_t i = 0; i < u->num_assets; i++)
        if (strncmp(u->assets[i].ticker, ticker, sizeof(u->assets[i].ticker)) == 0)
            return &u->assets[i];
    return NULL;
}
