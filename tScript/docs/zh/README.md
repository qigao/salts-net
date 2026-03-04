# TurboScript

**一门高性能脚本语言，支持 JIT 编译和可扩展插件系统。**

TurboScript 是一门面向生产环境的领域特定语言（DSL），专为数据处理、量化金融和通用脚本编写而设计。它具有现代化的语法、丰富的内置库，以及强大的插件架构，允许通过 C/C++ DLL 扩展语言功能。

---

## 🚀 快速链接

### 用户文档
- **[快速开始](getting-started.md)** - 5 分钟教程，编写你的第一个脚本
- **[语言指南](language-guide.md)** - 完整的语法参考和语言特性
- **[API 参考](api-reference.md)** - 内置函数和标准库

### 开发者文档
- **[插件开发](plugin-development.md)** - 使用 C/C++ 扩展 TurboScript
- **[模块文档](../modules/)** - 领域特定模块指南
- **[架构设计](../advanced/architecture.md)** - 内部设计和实现

### 速查表
- **[数学函数](../math_cheatsheet.md)** - 数学函数速查
- **[向量操作](../vec_cheatsheet.md)** - 向量/数组操作
- **[技术分析](../ta_fin_cheatsheet.md)** - TA 指标和金融函数

---

## ✨ 核心特性

### 现代化语言设计
- **动态类型**，支持类型内省（`typeof`、`is_number` 等）
- **箭头函数**和闭包：`(x) => x * 2`
- **解构赋值**：`let [a, b] = [10, 20]`
- **管道操作符**：`data |> filter(x > 0) |> sum()`
- **可选链**：`user?.address?.city`

### 丰富的数据类型
- **数字**：64 位浮点数
- **字符串**：UTF-8，支持模板字符串
- **向量**：高效的数值数组
- **映射**：基于哈希表的键值存储
- **列表**：异构集合

### 内置库
- **数学**：三角函数、统计、线性代数
- **字符串**：操作、解析、格式化
- **文件 I/O**：读写文件、目录操作
- **日期/时间**：解析、格式化、时间戳

### 可扩展架构
- **插件系统**：通过 `import("plugin_name")` 动态加载 C/C++ DLL
- **模块化**：清晰的命名空间分离（`csv.*`、`json.*`、`ta.*`）
- **JIT 编译**：通过 MIR 后端实现高性能执行
- **零拷贝 FFI**：与宿主应用程序高效数据交换

---

## 📦 安装

### 从源码构建
```bash
git clone https://github.com/your-org/turbonet.git
cd turbonet/tScript
mkdir build && cd build
cmake ..
make
```

### 使用预编译二进制
从 [Releases](https://github.com/your-org/turbonet/releases) 下载最新版本。

---

## 🎯 快速示例

```javascript
// 加载插件
import("csv");
import("ta");

// 读取和处理数据
var data = csv.read("prices.csv");
var close = csv.col(data, "close");

// 计算技术指标
var sma20 = ta.sma(close, 20);
var rsi14 = ta.rsi(close, 14);

// 生成信号
var signal = (rsi14 < 30) ? "买入" : (rsi14 > 70) ? "卖出" : "持有";

print("信号: " + signal);
```

---

## 🌍 语言支持

- **English**: [Primary documentation](../README.md)
- **中文**: 主文档（当前目录）

---

## 📚 模块生态系统

TurboScript 提供了丰富的可选模块：

| 模块 | 说明 | 文档 |
|--------|-------------|---------------|
| `csv` | CSV 解析和操作 | [csv_filter_expression.md](../csv_filter_expression.md) |
| `json` | JSON 解析和查询 | [modules/json.md](../modules/json.md) |
| `ta` | 技术分析指标 | [ta_fin_cheatsheet.md](../ta_fin_cheatsheet.md) |
| `vec` | 高级向量操作 | [vec_cheatsheet.md](../vec_cheatsheet.md) |
| `net` | HTTP/WebSocket 网络 | [modules/net.md](../modules/net.md) |
| `sqlite` | 数据库访问 | [modules/sqlite.md](../modules/sqlite.md) |
| `finance` | 投资组合优化、因子分析 | [modules/finance.md](../modules/finance.md) |
| `wasm` | WebAssembly 执行 | [../../modules/wasm/README.md](../../modules/wasm/README.md) |

---

## 🛠️ 开发

### 项目结构
```
tScript/
├── exprtk/          # 核心解释器
├── ts_loader/       # 插件加载器
├── modules/         # 内置和插件模块
├── docs/            # 文档（你在这里）
└── tests/           # 测试套件
```

### 贡献
参见 [CONTRIBUTING.md](../../CONTRIBUTING.md) 了解贡献指南。

---

## 📄 许可证

[Your License Here]

---

## 🤝 社区

- **问题反馈**：[GitHub Issues](https://github.com/your-org/turbonet/issues)
- **讨论**：[GitHub Discussions](https://github.com/your-org/turbonet/discussions)
- **Discord**：[加入我们的服务器](https://discord.gg/your-invite)

---

**为高性能脚本而生 ❤️**
