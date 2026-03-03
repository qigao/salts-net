# TurboScript TA & Finance (fin) Cheat Sheet

面向量化金融、策略编写和技术分析的速查表。
TurboScript 提供专为高性能回测和交易设计的双引擎模块：`ta` (技术分析与定价) 和 `fin` (`strategy.*` 交易上下文与统计分析)。

> **提示:** 这些模块属于外部插件。在脚本运行前请确保通过 `import("ta")` 和 `import("fin")` 加载，或在宿主程序中 `turbo_script_load_plugin(ctx, "ta")`。

## 技术指标 (TA)

所有技术指标原生支持向量操作 (SIMD 优化)，支持点号调用 `v.sma(14)`。

| 函数 / 别名 | 说明 | 示例 |
|---|---|---|
| `ta.sma(v, period)` | 简单移动平均 | `close.sma(14)` |
| `ta.ema(v, period)` | 指数移动平均 | `ta.ema(close, 20)` |
| `ta.macd(v, fast, slow, sig)` | 平滑异同移动平均线 | `var [macd, sig, hist] = close.macd(12, 26, 9)` |
| `ta.rsi(v, period)` | 相对强弱指数 | `close.rsi(14)` |
| `ta.bbands(v, period, dev)` | 布林带 | `var [up, mid, dn] = close.bbands(20, 2.0)` |
| `ta.atr(h, l, c, period)` | 真实波动幅度 | `ta.atr(high, low, close, 14)` |
| `ta.stoch(h, l, c, k, d)` | 随机指标 (KDJ) | `ta.stoch(high, low, close, 9, 3)` |
| `ta.candle_doji(o, h, l, c)` | 十字星形态识别 | `ta.candle_doji(open, high, low, close)` |
| `ta.crossover(v1, v2)` | 金叉判定 (v1 上穿 v2) | `ta.crossover(sma5, sma10)` |
| `ta.crossunder(v1, v2)` | 死叉判定 (v1 下穿 v2) | `ta.crossunder(sma5, sma10)` |

### 期权定价 (Black-Scholes-Merton)

| 函数 | 说明 | 示例 (S:标的, K:行权, T:时间, r:利率, sigma:波动率) |
|---|---|---|
| `ta.bsm_call(S, K, T, r, v)` | 看涨期权定价 | `ta.bsm_call(100, 105, 0.5, 0.02, 0.2)` |
| `ta.bsm_put(S, K, T, r, v)` | 看跌期权定价 | `ta.bsm_put(100, 95, 0.5, 0.02, 0.2)` |
| `ta.bsm_iv_call(...)` | 看涨隐含波动率求导 | `ta.bsm_iv_call(price, S, K, T, r)` |
| `ta.bsm_delta_call(...)` | 希腊字母 Delta (Call) | `ta.bsm_delta_call(S, K, T, r, v)` |

---

## 策略上下文与执行 (fin / strategy)

`strategy.*` 提供了一套完整的环境来进行信号发生、头寸管理与绩效评估。

### 交易执行

| 函数 | 说明 | 示例 |
|---|---|---|
| `strategy.buy(size)` | 做多 / 平空 (指定数量) | `strategy.buy(1.0)` |
| `strategy.sell(size)` | 做空 / 平多 (指定数量) | `strategy.sell(1.0)` |
| `strategy.flat()` | 清仓 (平掉所有当前持仓) | `strategy.flat()` |
| `strategy.set_sl(price)` | 设置当前仓位的止损价 | `strategy.set_sl(entry_px - atr * 2)` |
| `strategy.set_tp(price)` | 设置当前仓位的止盈价 | `strategy.set_tp(entry_px + atr * 3)` |

### 仓位与状态

| 函数 | 说明 | 示例 |
|---|---|---|
| `strategy.pos()` | 获取当前持仓数量 (正多负空) | `if (strategy.pos() == 0) { ... }` |
| `strategy.is_long()` | 是否持有多头 | `if (strategy.is_long()) { ... }` |
| `strategy.is_short()`| 是否持有空头 | `if (strategy.is_short()) { ... }` |
| `strategy.is_flat()` | 是否空仓 | `if (strategy.is_flat()) { ... }` |
| `strategy.entry_px()`| 当前仓位的平均开仓价 | `var entry = strategy.entry_px()` |

### 绩效评估统计

| 函数 | 说明 |
|---|---|
| `strategy.cum_pnl()` | 获取策略当前累计收益 (Cumulative PnL) |
| `strategy.unrealized_pnl()` | 获取当前仓位的未实现盈亏 (浮盈/浮亏) |
| `strategy.num_trades()` | 获取完成的交易次数 |
| `strategy.win_rate()` | 胜率 (盈利记录数 / 总记录数) |
| `strategy.sharpe()` | 夏普比率 (风险调整后收益) |
| `strategy.max_dd_duration()` | 最大回撤持续时间 |
| `strategy.kelly_size()` | 计算凯利公式推荐的最佳头寸比例 |

---

## 组合示例：完整的趋势交叉策略

```js
// 1. 获取行情并计算指标
var sma_fast = close.sma(10);
var sma_slow = close.sma(30);

// 2. 状态获取
var is_flat = strategy.is_flat();
var is_long = strategy.is_long();

// 3. 交易逻辑
if (ta.crossover(sma_fast, sma_slow)) {
    // 金叉做多
    if (!is_long) {
        strategy.flat();
        strategy.buy(100); 
    }
} else if (ta.crossunder(sma_fast, sma_slow)) {
    // 死叉做空或平仓
    if (is_long) {
        strategy.flat();
    }
}

// 4. 风控与止损
if (!is_flat) {
    var entry = strategy.entry_px();
    var risk  = ta.atr(high, low, close, 14) * 2;
    if (is_long) {
        strategy.set_sl(entry - risk);
    }
}
```
