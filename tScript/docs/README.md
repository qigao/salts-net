# TurboScript

**A high-performance scripting language with JIT compilation and extensible plugin system.**

TurboScript is a production-ready DSL designed for data processing, quantitative finance, and general-purpose scripting. It features modern syntax, rich built-in libraries, and a powerful plugin architecture that allows extending the language via C/C++ DLLs.

---

## 🚀 Quick Links

### For Users
- **[Getting Started](getting-started.md)** - 5-minute tutorial to write your first script
- **[Language Guide](language-guide.md)** - Complete syntax reference and language features
- **[API Reference](api-reference.md)** - Built-in functions and standard library

### For Developers
- **[Plugin Development](plugin-development.md)** - Extend TurboScript with C/C++ plugins
- **[Module Documentation](modules/)** - Domain-specific module guides
- **[Architecture](advanced/architecture.md)** - Internal design and implementation

### Quick Reference
- **[Math Cheatsheet](math_cheatsheet.md)** - Mathematical functions
- **[Vector Operations](vec_cheatsheet.md)** - Vector/array operations
- **[Technical Analysis](ta_fin_cheatsheet.md)** - TA indicators and finance functions

---

## ✨ Key Features

### Modern Language Design
- **Dynamic typing** with type introspection (`typeof`, `is_number`, etc.)
- **Arrow functions** and closures: `(x) => x * 2`
- **Destructuring assignment**: `let [a, b] = [10, 20]`
- **Pipe operator**: `data |> filter(x > 0) |> sum()`
- **Optional chaining**: `user?.address?.city`

### Rich Data Types
- **Numbers**: 64-bit floating point
- **Strings**: UTF-8 with template literals
- **Vectors**: Efficient numeric arrays
- **Maps**: Hash-based key-value stores
- **Lists**: Heterogeneous collections

### Built-in Libraries
- **Math**: Trigonometry, statistics, linear algebra
- **String**: Manipulation, parsing, formatting
- **File I/O**: Read/write files, directory operations
- **Date/Time**: Parsing, formatting, timestamps

### Extensible Architecture
- **Plugin system**: Load C/C++ DLLs dynamically via `import("plugin_name")`
- **Module-based**: Clean namespace separation (`csv.*`, `json.*`, `ta.*`)
- **JIT compilation**: High-performance execution via MIR backend
- **Zero-copy FFI**: Efficient data exchange with host applications

---

## 📦 Installation

### From Source
```bash
git clone https://github.com/your-org/turbonet.git
cd turbonet/tScript
mkdir build && cd build
cmake ..
make
```

### Using Pre-built Binaries
Download the latest release from [Releases](https://github.com/your-org/turbonet/releases).

---

## 🎯 Quick Example

```javascript
// Load plugins
import("csv");
import("ta");

// Read and process data
var data = csv.read("prices.csv");
var close = csv.col(data, "close");

// Calculate technical indicators
var sma20 = ta.sma(close, 20);
var rsi14 = ta.rsi(close, 14);

// Generate signals
var signal = (rsi14 < 30) ? "BUY" : (rsi14 > 70) ? "SELL" : "HOLD";

print("Signal: " + signal);
```

---

## 🌍 Language Support

- **English**: Primary documentation (this directory)
- **中文**: [Chinese translation](zh/README.md)

---

## 📚 Module Ecosystem

TurboScript comes with a rich set of optional modules:

| Module | Description | Documentation |
|--------|-------------|---------------|
| `csv` | CSV parsing and manipulation | [csv_filter_expression.md](csv_filter_expression.md) |
| `json` | JSON parsing and querying | [modules/json.md](modules/json.md) |
| `ta` | Technical analysis indicators | [ta_fin_cheatsheet.md](ta_fin_cheatsheet.md) |
| `vec` | Advanced vector operations | [vec_cheatsheet.md](vec_cheatsheet.md) |
| `net` | HTTP/WebSocket networking | [modules/net.md](modules/net.md) |
| `sqlite` | Database access | [modules/sqlite.md](modules/sqlite.md) |
| `finance` | Portfolio optimization, factor analysis | [modules/finance.md](modules/finance.md) |
| `wasm` | WebAssembly execution | [../../modules/wasm/README.md](../../modules/wasm/README.md) |

---

## 🛠️ Development

### Project Structure
```
tScript/
├── exprtk/          # Core interpreter
├── modules/         # Built-in and plugin modules
├── docs/            # Documentation (you are here)
└── tests/           # Test suite
```

### Contributing
See [CONTRIBUTING.md](../CONTRIBUTING.md) for guidelines.

---

## 📄 License

[Your License Here]

---

## 🤝 Community

- **Issues**: [GitHub Issues](https://github.com/your-org/turbonet/issues)
- **Discussions**: [GitHub Discussions](https://github.com/your-org/turbonet/discussions)
- **Discord**: [Join our server](https://discord.gg/your-invite)

---

**Built with ❤️ for high-performance scripting**
