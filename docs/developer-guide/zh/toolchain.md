# 安装工具链

seekdb 使用 Bazel、Rust 以及仓库管理的编译器和依赖集合。先安装 Rust 和少量宿主机工具，再由 `./build.py init` 将固定版本的 C/C++ 构建依赖准备到 `deps/3rd`。

## Rust

通过 [rustup](https://rustup.rs/) 安装 Rust。在 Linux、macOS 或 WSL 上，执行官方安装脚本，并让 Cargo 环境在当前 shell 中生效：

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source "$HOME/.cargo/env"
```

> **中国大陆网络：** 如果官方源下载缓慢，可以在当前终端使用[清华大学 TUNA rustup 镜像](https://mirrors.tuna.tsinghua.edu.cn/help/rustup/)，然后执行安装命令：

```bash
export RUSTUP_DIST_SERVER=https://mirrors.tuna.tsinghua.edu.cn/rustup
export RUSTUP_UPDATE_ROOT=https://mirrors.tuna.tsinghua.edu.cn/rustup/rustup
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source "$HOME/.cargo/env"
```

上述环境变量也会让同一终端中后续的 rustup 工具链下载使用 TUNA 镜像。首次构建还需要下载 Cargo 依赖；如果 crates.io 访问缓慢，可以在 `$HOME/.cargo/config.toml` 中配置 [TUNA crates.io 稀疏索引](https://mirrors.tuna.tsinghua.edu.cn/help/crates.io-index/)：

```toml
[source.crates-io]
replace-with = "tuna"

[source.tuna]
registry = "sparse+https://mirrors.tuna.tsinghua.edu.cn/crates.io-index/"

[registries.tuna]
index = "sparse+https://mirrors.tuna.tsinghua.edu.cn/crates.io-index/"
```

在 Windows 上，从 [Rust 官方安装页面](https://www.rust-lang.org/tools/install) 下载并运行 `rustup-init.exe`，然后重新打开 PowerShell，使 `%USERPROFILE%\.cargo\bin` 出现在 `PATH` 中。

仓库固定使用 Rust `1.98.1`，需要 `clippy` 组件，并声明以下目标：

```text
x86_64-unknown-linux-gnu
x86_64-pc-windows-gnu
aarch64-linux-android
aarch64-apple-ios
aarch64-apple-ios-sim
```

编译前先在 `rust/` 目录执行 `rustup toolchain install --no-self-update`，安装固定版本的工具链、组件和目标。Bazel 查找已安装的工具链，并通过 Cargo 生成 Rust 库和 C++ 头文件。首次构建还可能下载 Cargo 的锁定依赖；如果主机无法访问默认 registry，请配置 crates.io 镜像。

## 宿主机检测与架构

当前依赖初始化脚本可以识别以下宿主机系列。能够选择依赖配置不代表该平台生成的所有软件包都已通过生产认证。

| 架构 | 可识别的 Linux 系列 |
| --- | --- |
| x86_64 | RHEL、CentOS、AlmaLinux、Rocky Linux、Alibaba Cloud Linux/AliOS、Anolis OS、TencentOS、Ubuntu、Debian、Fedora、Kylin、openEuler、openSUSE Leap、SLES 和 UOS |
| aarch64 | RHEL、CentOS、AlmaLinux、Rocky Linux、Alibaba Cloud Linux/AliOS、Anolis OS、Ubuntu、Debian、Kylin 和 openEuler |

不能根据 x86_64 列表推断 Fedora、openSUSE/SLES、UOS 或 TencentOS 的 aarch64 支持；当前 aarch64 依赖分支不会选择这些宿主机。

兼容构建还可以识别 macOS 13 或更高版本的 `arm64` 和 `x86_64`。Apple Silicon 是主要测试的开发平台；构建脚本接受 Intel 主机并不等同于生产支持承诺。

Windows 11 x64 使用 `python build.py`，`init` 会选择 Windows 依赖初始化脚本。

## Linux 宿主机依赖

### RHEL 兼容系统

```bash
sudo yum install git wget curl rpm-build rpm2cpio cpio make glibc-devel glibc-headers binutils m4 libtool libaio python3
```

### Debian 兼容系统

```bash
sudo apt-get update
sudo apt-get install git wget curl rpm rpm2cpio cpio make build-essential binutils m4 file python3
```

Ubuntu 24.04 和 Debian 13 使用 time64 版本的 libaio：

```bash
sudo apt-get install libaio1t64
```

### SUSE 兼容系统

```bash
sudo zypper install git wget curl rpm cpio make glibc-devel binutils m4 python3
```

## macOS 宿主机依赖

```bash
brew install git cmake pkg-config openssl@3 ncurses googletest
brew install zstd lz4 utf8proc thrift re2 brotli
```

需要使用镜像时，参见 [Homebrew 优化配置](homebrew.md)。

### macOS 27（Apple Silicon）的 CMake 兼容构建

macOS 27 及以上使用独立的 `oceanbase.macos27.arm64.deps`，不再下载 LLVM。
第三方库暂时复用 macOS 15 仓库中的包。请先安装带 macOS 27 SDK 的 Xcode 或
Command Line Tools；CMake 优先检查当前选中的开发工具，如果 SDK 太旧，再检查
`/Library/Developer/CommandLineTools`。默认使用选中工具集的 Apple Clang。

```bash
./build.py init
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_release -j8
```

可以用 `-DOB_MACOS_DEVELOPER_DIR=/path/to/Contents/Developer` 指定开发工具，
或在首次配置时设置 `DEVELOPER_DIR`。显式选择的工具必须提供 SDK 27 或更新版本。
选定的 SDK、链接器及 Rust/Cargo 开发工具环境会写入构建规则，后续构建无需再次设置环境变量。

如果希望使用 Homebrew LLVM（需兼容 SDK 27 的 C++ 标准库头文件）：

```bash
brew install llvm
./build.py init
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_MACOS_LLVM_ROOT="$(brew --prefix llvm)"
```

此选项选择 Homebrew 的 Clang/Clang++，SDK 和链接器仍来自上述 Apple 开发工具。
配置阶段会编译并链接一个使用 C++ 标准库的程序，以检查 LLVM 与 SDK 的兼容性。
切换编译器时，使用新的 CMake 构建目录。

## Windows 宿主机依赖

安装 Python 3 和 Visual Studio 2022 Build Tools，并选择 **使用 C++ 的桌面开发** 工作负载。随后初始化仓库管理的 CMake、Ninja、LLVM、win_flex_bison、OpenSSL 和第三方库：

```powershell
python .\build.py init
```

.NET 8 和 WiX v4 是可选依赖，仅配置器和 MSI 打包流程需要。

## 验证环境

回到仓库根目录执行：

```bash
./build.py init
./build.py version
```

然后继续阅读[获取代码、编译并运行 seekdb](build-and-run.md)。
