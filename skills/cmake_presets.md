# CMake Presets 使用指南

## 概述

本仓库使用 CMake Presets 作为标准构建入口。日常 configure、build、test 必须优先走 preset，不直接手写 `cmake -S . -B ...`，除非正在修改 preset 本身。

**主要文件**：
- `CMakePresets.json`：仓库共享 preset 入口，组合 `presets/BuildPresets.json` 和 `presets/TestPresets.json`。
- `CMakeUserPresets.json`：本机路径与用户级入口，包含 `win-dev-user`、`win-release-user`、`linux-dev-user`、`linux-release-user`。
- `presets/*.json`：隐藏/base preset、平台条件、编译器、flags、选项。

---

## 发现可用 Preset

先查询当前平台实际可用入口：

```bash
cmake --list-presets
cmake --build --list-presets
ctest --list-presets
```

在 Windows 当前用户环境中，日常优先使用：

```bash
cmake --fresh --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user
```

开发/ASan 场景使用：

```bash
cmake --fresh --preset win-dev-user
cmake --build --preset win-dev-user
ctest --preset win-dev-user
```

Linux 用户入口：

```bash
cmake --fresh --preset linux-release-user
cmake --build --preset linux-release-user
ctest --preset linux-release-user
```

---

## Windows 环境

Windows 下必须先进入 VS toolchain 环境。自动化命令统一用 `cmd /c` 调 `VsDevCmd.bat`，不要从裸 PowerShell 直接跑 MSVC/Ninja 构建。

```bash
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 >nul && cmake --fresh --preset win-release-user"
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 >nul && cmake --build --preset win-release-user"
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 >nul && ctest --preset win-release-user"
```

如果本机 VS 安装路径不同，先用 `vswhere` 或本机实际路径确认，不要硬编码到仓库 preset。

模板：

```bash
cmd /c "call ""<VS>\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 >nul && <cmake-or-ctest-command>"
```

---

## 构建范围

### 全量构建

```bash
cmake --build --preset win-release-user
```

### 构建指定 target

```bash
cmake --build --preset win-release-user --target turbo_utils
cmake --build --preset win-release-user --target test_fmt test_turbo_error
cmake --build --preset win-release-user --target test_stream test_datagram
```

### 运行测试

优先用 CTest preset 跑测试集合：

```bash
ctest --preset win-release-user
ctest --preset win-release-user -R test_turbo_error
ctest --preset win-release-user -R test_stream
```

需要调试 TinyTest 过滤器或查看单测输出时，可直接运行生成的 exe：

```bash
build\Msvc-Release\bin\test_turbo_error.exe
build\Msvc-Release\bin\test_stream.exe --filter "without use-after-free"
```

---

## Preset 选择规则

- 普通 Windows 验证：`win-release-user`。
- 需要 ASan/开发模式：`win-dev-user`。
- 普通 Linux 验证：`linux-release-user`。
- Linux 开发/ASan：`linux-dev-user`。
- SDK 打包：`linux-sdk-package`。
- 不直接使用 hidden/base preset，例如 `release-win-msvc-ninja`、`base-win-release-user`。
- 不把机器本地路径写入共享 `CMakePresets.json`；本机路径放在 `CMakeUserPresets.json`。

---

## 生成目录

当前 preset 默认生成目录：

| Preset | Binary dir |
|--------|------------|
| `win-dev-user` | `build/Msvc` |
| `win-release-user` | `build/Msvc-Release` |
| `linux-dev-user` | `build/linux-gcc-debug` |
| `linux-release-user` | `build/linux-gcc-release` |
| `linux-sdk-package` | `build/linux-gcc-sdk-package` |

不要混用同一个 build 目录跑不同 compiler、generator 或 preset。

---

## 恢复损坏的 Build Tree

优先使用 `--fresh` 重新 configure：

```bash
cmake --fresh --preset win-release-user
```

适用场景：
- compiler path 变化
- `CMakeCache.txt` 中 generator/toolchain 和当前 preset 不一致
- `build.ninja` 缺失或 `CMakeFiles/rules.ninja` 缺失
- vcpkg package config 曾经未生成，后续已安装成功

只有 `--fresh` 仍无法恢复时，才考虑删除对应生成目录。删除前必须确认解析后的绝对路径在仓库 `build/` 下，例如：

```powershell
$target = Resolve-Path build\Msvc-Release
```

确认无误后才可删除该生成目录；不要删除源码目录、`vcpkg_installed` 或用户数据。

---

## 修改 Preset 的规则

- 新增跨机器共享配置时改 `presets/*.json` 或 `CMakePresets.json`。
- 新增本机路径、安装目录、工具路径时改 `CMakeUserPresets.json`。
- 改 preset 后必须至少运行：

```bash
cmake --list-presets
cmake --build --list-presets
ctest --list-presets
cmake --fresh --preset win-release-user
```

- 修改构建选项影响全仓库时，再跑相关 target 或全量构建。

---

## 常见陷阱

- 不要调用 hidden preset 作为日常入口；`cmake --preset release-win-msvc-ninja` 可能不可用。
- 不要在损坏 build tree 上直接 `cmake --build`，先 `cmake --fresh --preset ...`。
- 不要手写 `-B build/Msvc-Release` 绕过 preset；这样会漏掉 vcpkg、prefix、compiler flags 或 generator。
- 不要把 Debug/Release、MSVC/GCC、Ninja/Visual Studio 生成结果混在同一目录。
- 不要把测试是否通过等同于 configure 成功；新文件通过 glob 收集时，必须重新 configure。

---

**最后更新**：2026-07-10
**适用项目**：TurboNet/TurboScript CMake preset 构建、测试与打包
