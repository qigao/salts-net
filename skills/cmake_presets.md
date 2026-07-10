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

## 安装与本机 Setup

本仓库的 install prefix 由 `CMakeUserPresets.json` 提供：

| Preset family | Install prefix |
|---------------|----------------|
| Windows user presets | `C:/projects/cpp/external/pkgs/turboutils` |
| Linux user presets | `/opt/turboutils` |

普通本机 setup 使用 Release preset；不要用 Debug/ASan 覆盖 Release SDK，除非明确需要调试安装包。

Windows Release 安装：

```bash
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 >nul && cmake --fresh --preset win-release-user && cmake --build --preset win-release-user && cmake --install build\Msvc-Release"
```

Linux Release 安装：

```bash
cmake --fresh --preset linux-release-user
cmake --build --preset linux-release-user
cmake --install build/linux-gcc-release
```

需要 Debug/ASan 安装时必须显式使用独立 prefix，避免覆盖 Release：

```bash
cmake --install build\Msvc --prefix C:\projects\cpp\external\pkgs\turboutils-debug
```

安装后消费端通过 CMake package 使用：

```cmake
find_package(TurboUtils CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE TurboUtils::Core)
```

消费端 configure 时把安装前缀放入 `CMAKE_PREFIX_PATH`：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/projects/cpp/external/pkgs/turboutils
```

---

## CMake Helper 使用规则

本仓库的一方模块优先使用 `cmake/CmakeUtils.cmake` 中的 helper，避免每个目录重复手写 target、CTest 和安装规则：

| 场景 | 默认使用 |
|------|----------|
| 收集源码 | `cmake_add_source()` |
| 配置库/可执行文件属性、alias、安装 | `cmake_config_target()` |
| Lemon/re2c 代码生成 | `cmake_add_grammar()` |
| 单测 target + CTest 注册 | `cmake_add_test()` |
| benchmark target | `cmake_add_benchmark()` |
| 安装头文件目录 | `cmake_install_headers()` |

推荐模式：

```cmake
cmake_add_source(SOURCE_FILES RECURSE)

add_library(my_module ${SOURCE_FILES})
target_link_libraries(my_module PUBLIC TurboUtils::Core)
cmake_config_target(my_module
  ALIAS TurboUtils::MyModule
  FOLDER "my_module")

cmake_add_test(
  SOURCES tests/test_my_module.c
  LIBS my_module TurboUtils::TinyTest
  FOLDER "my_module/tests")
```

例外：
- `vendor/` 上游 CMake、外部示例或第三方工程保持原生写法。
- helper 无法表达的自定义 target 名、特殊生成文件依赖、非标准测试命令，可以直接写原生 CMake，但创建 target 后仍调用 `cmake_config_target(... NO_INSTALL FOLDER ...)`。
- 测试、benchmark、examples、内部工具默认 `NO_INSTALL`；公开库才进入 `TurboUtilsTargets` 导出集。

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
**适用项目**：TurboUtils CMake preset 构建、测试与打包
