# seekdb Bazel 版本、国内安装与编译平台调研

调研日期：2026-10-16。仓库基线：`a8afc89e9`。平台调研阶段阅读仓库配置及官方资料，没有做跨平台编译实测；随后按要求完成了 Linux x86_64 的 Bazel 8.2.1 和 9.3.0 远程构建，记录见文末。

主要外部来源：[Bazel 正式发布](https://github.com/bazelbuild/bazel/releases)、[Bazelisk 官方说明](https://github.com/bazelbuild/bazelisk)、[华为云 Bazel 镜像目录](https://mirrors.huaweicloud.com/bazel/)、[Bazel 平台模型](https://bazel.build/concepts/platforms)。下文区分 seekdb 现有构建入口、当前 Bazel 接入程度和 Bazel 生态能力。

## 版本结论

| 项目 | 版本 | 依据 |
| --- | --- | --- |
| 当前 seekdb 固定版本 | **9.3.0** | [`.bazelversion`](../../.bazelversion)、[`MODULE.bazel`](../../MODULE.bazel) 同时限定 `>=9.3.0`、`<=9.3.0`；[`bazel.py`](../../bazel.py) 的 `_verify_bazel_version` 要求完全一致 |
| 最新稳定版本 | **9.3.0**，2026-10-07 发布 | [官方 9.3.0 release](https://github.com/bazelbuild/bazel/releases/tag/9.3.0)，标记为 Latest，属于 minor LTS release |
| 最新候选版本 | **10.0.0rc1**，2026-10-07 发布 | [官方发布列表](https://github.com/bazelbuild/bazel/releases)，标记为 Pre-release |
| 8.x 最新维护版本 | **8.8.1**，2026-09-24 发布 | [官方 8.8.1 release](https://github.com/bazelbuild/bazel/releases/tag/8.8.1) |

当前启动器在找不到 Bazel 时会自动安装 9.3.0，也可使用已安装的同版本程序。已完成 Linux x86_64 远程编译验证，并将新锁文件和 Cargo PATH 配置纳入默认构建；当前版本检查会拒绝其他版本。官方建议通过 Bazelisk 管理项目版本。[官方安装建议](https://bazel.build/install/bazelisk)

## 国内安装与工具链供应

推荐先使用下文“仅针对 Bazel 自身的无代理使用”中的 `python3 bazel.py --no-proxy` 入口，以下 Bazelisk 和手动安装方式仍可选用。

个人和团队使用同一套 Bazel/Bazelisk。Bazelisk 负责读取仓库的版本号、下载和启动对应 Bazel；团队增加的工作是维护下载源和公共缓存，开发者不必各自搭服务器。开源项目提供公开的国内镜像时，个人贡献者也能直接使用。

**先区分两个下载：安装 Bazelisk 启动器，和启动器随后下载 Bazel 9.3.0。** 已有 Node.js/npm 的开发者可以从国内 npm 镜像安装启动器：

```bash
npm install -g @bazel/bazelisk@1.28.1 --registry=https://registry.npmmirror.com
cd /path/to/seekdb
bazel --version
```

本次查询确认 [npmmirror 上的 1.28.1 元数据](https://registry.npmmirror.com/@bazel/bazelisk/1.28.1)和 npm 包地址均存在。官方 npm 包包含六种宿主平台的 Bazelisk 二进制，安装包没有 postinstall 再从 GitHub 下载启动器的步骤；npm 包装器需要 Node.js，独立 Bazelisk 二进制不需要。`bazel --version` 首次运行时，启动器读取本仓库的 `9.3.0`，再下载实际 Bazel，后续复用本地缓存。[官方打包规则](https://github.com/bazelbuild/bazelisk/blob/v1.28.1/BUILD)、[官方启动脚本](https://github.com/bazelbuild/bazelisk/blob/v1.28.1/bazelisk.js)、[版本选择机制](https://github.com/bazelbuild/bazelisk/blob/v1.28.1/README.md)

**国内 npm 镜像只解决第一步。** 本次对华为 `https://mirrors.huaweicloud.com/bazel/9.3.0/bazel-9.3.0-linux-x86_64` 的直接请求返回 404，不能继续把华为源当作当前项目的完整安装方案。官方 `releases.bazel.build` 在本机可直连，但下载较慢；这不代表已验证国内所有网络均能稳定使用。项目或团队供应对应平台的二进制镜像，可以统一第二步的下载地址。

**接受稍旧版本时，华为云已有覆盖六种宿主平台的 Bazel 9.2.0 公共镜像。** [9.2.0 目录](https://mirrors.huaweicloud.com/bazel/9.2.0/)包含 Linux、macOS、Windows 各自的 x86_64 和 ARM64 二进制及 SHA256 文件。本次对六个非 `nojdk` 二进制地址分别使用 `curl --noproxy '*' --head` 检查，全部返回 HTTP 200，没有下载或执行这些二进制。

| 宿主系统 | x86_64 制品 | ARM64 制品 |
| --- | --- | --- |
| Linux | `bazel-9.2.0-linux-x86_64` | `bazel-9.2.0-linux-arm64` |
| macOS | `bazel-9.2.0-darwin-x86_64` | `bazel-9.2.0-darwin-arm64` |
| Windows | `bazel-9.2.0-windows-x86_64.exe` | `bazel-9.2.0-windows-arm64.exe` |

Bazelisk 可设置 `BAZELISK_BASE_URL=https://mirrors.huaweicloud.com/bazel`，再由项目选择镜像中已有的版本。当前 seekdb 仍固定且已验证 9.3.0，因此仅设置该地址不能让当前项目自动使用 9.2.0；改用 9.2.0 需要同步版本限制、锁文件并重新验证构建。本次只补充镜像调研，没有降级项目。这六种是 Bazel 的宿主平台，不表示 seekdb 已在它们上面完成适配，也不包含 Android、iOS、Wasm 交叉编译 SDK。

**Ubuntu/Debian x86_64 还有已经同步 9.3.0 的国内路径：清华 Bazel APT 源。** 本次直接查询其 [Packages 索引](https://mirrors.tuna.tsinghua.edu.cn/bazel-apt/dists/stable/jdk1.8/binary-amd64/Packages)，确认 `bazel` 和 `bazel-9.3.0` 两个包均含版本 9.3.0。按[清华配置说明](https://mirrors.tuna.tsinghua.edu.cn/help/bazel-apt/)添加签名密钥和软件源后，可以执行：

```bash
sudo apt update
sudo apt install bazel=9.3.0
```

此方式直接安装 Bazel，无需 Bazelisk；适用于上述 APT 平台，不覆盖 macOS、Windows 或当前 Alibaba Cloud Linux 主机。这里只核验了索引和包地址，没有在本机执行 APT 安装。

不使用 Bazelisk 时，也可安装单个 Bazel 文件。以下是 **Linux x86_64 示例**，下载时只对当前命令绕过代理；其他平台应选官方对应制品：

```bash
(
  set -eu
  bazel_install_tmp=$(mktemp -d)
  trap 'rm -rf "$bazel_install_tmp"' EXIT
  cd "$bazel_install_tmp"
  bazel_file=bazel-9.3.0-linux-x86_64
  bazel_source=https://releases.bazel.build/9.3.0/release
  curl --noproxy '*' -fLO "$bazel_source/$bazel_file"
  curl --noproxy '*' -fLO "$bazel_source/$bazel_file.sha256"
  sha256sum -c "$bazel_file.sha256"
  install -Dm755 "$bazel_file" "$HOME/.local/bin/bazel"
)
export PATH="$HOME/.local/bin:$PATH"
```

以上 SHA256 核对下载文件与官方清单是否一致。普通发行版携带 Java 运行时，运行 Bazel 本身不要求再安装 Java；C/C++ 编译器、Rust 和平台 SDK 仍需单独准备。[Bazel Java 运行时说明](https://bazel.build/install/ubuntu)

团队长期供应可以用现有制品库、国内对象存储或静态 HTTP 服务，无需为下载二进制部署远程编译服务。维护者负责从官方取得所需平台的 Bazelisk 和 Bazel 文件、核对 SHA256，并把 Bazel 按下面的目录发布：

```text
bazel/
  9.3.0/
    bazel-9.3.0-linux-x86_64
    bazel-9.3.0-linux-arm64
    bazel-9.3.0-darwin-arm64
    ...其余团队支持的平台，以及校验文件
```

例如维护者将该目录发布为 `https://artifacts.example.com/bazel` 后，开发者仍从上述 npm 镜像安装 Bazelisk，另设置：

```bash
# 示意地址：需要替换为团队实际部署的地址。
export BAZELISK_BASE_URL=https://artifacts.example.com/bazel
cd /path/to/seekdb
bazel --version
```

Linux x86_64 开发者随后取得的文件就是 `https://artifacts.example.com/bazel/9.3.0/bazel-9.3.0-linux-x86_64`。安装脚本可封装环境配置、版本检查和校验，成员只需执行安装脚本；这一步是建议方案，本次未部署该服务。团队也可以直接在安装脚本中分发独立 Bazelisk，省去 Node.js/npm。开源个人贡献者应能访问公开的镜像地址，只有公司内网可访问的制品库不能解决他们的安装问题。

`BAZELISK_BASE_URL` 按 `/<版本>/<文件名>` 拼接下载地址；非标准目录可用 `BAZELISK_FORMAT_URL`，两者不同时设置。`BAZELISK_VERIFY_SHA256` 可固定当前平台制品校验值。[官方 README](https://github.com/bazelbuild/bazelisk/blob/v1.28.1/README.md)

完整的无代理开发还需覆盖另外几层下载：

仅镜像 Bazel 二进制不足以完成首次构建。普通发布包自带运行 Bazel 的 Java 运行时，但构建规则和模块是另一层外部依赖，其中部分由 Bazel 自动引入。[Bazel 9 发布说明](https://github.com/bazelbuild/bazel/releases/tag/9.0.0)明确要求 protobuf 至少为 33.4；本仓库当前锁文件也含 `protobuf/33.4/source.json`。这类下载不受 `BAZELISK_BASE_URL` 控制，也不是手动安装系统 protobuf 包就能替代。

| 下载层 | 当前项目条件与建议 |
| --- | --- |
| Bazel/Bazelisk 本体 | 维护者同步国内制品镜像；开发者从同一入口获取启动器和仓库固定的 9.3.0 |
| BCR 元数据、规则源码与依赖归档 | 仓库已有 [`tools/bazel_registry.py`](../../tools/bazel_registry.py)，可依据 `MODULE.bazel.lock` 准备内部 registry 并预热源码归档镜像；[`bazel.py`](../../bazel.py) 接收成对的 `SEEKDB_BAZEL_REGISTRY`/`SEEKDB_BAZEL_MIRROR` 或 `--registry`/`--registry-mirror` |
| C/C++ 编译器及第三方库 | Linux 沿用 seekdb 的 `deps` 初始化和 `mirrors.oceanbase.com` 包，主要是 LLVM 17.0.6、GCC 12.3；macOS 27 走本机 Apple Clang/SDK，不能照搬 Linux 工具链 |
| Rust、Cargo 与交叉编译 SDK | 分别准备固定 Rust toolchain、Cargo registry/源码缓存，以及 NDK、Xcode SDK、Emscripten；Bazel 二进制镜像不会自动覆盖它们 |

Rust 可使用 [TUNA rustup 镜像](https://mirrors.tuna.tsinghua.edu.cn/help/rustup/)，仓库的[工具链文档](../developer-guide/zh/toolchain.md)也给出了 TUNA Cargo sparse registry 配置。macOS 的 Homebrew 安装程序、索引及 bottles 可按 [TUNA Homebrew 说明](https://mirrors.tuna.tsinghua.edu.cn/help/homebrew/)配置镜像。当前原生构建固定 Rust `1.98.1`，Wasm 固定 `nightly-2026-09-07`；TUNA 只保留一段时间的 nightly，因此团队应缓存所需的固定 nightly。Xcode/Windows SDK 从各自厂商渠道准备，NDK/Emscripten 及其下载内容需另行镜像，当前没有据此验证全平台依赖均能只靠公共镜像完成安装。[原生 Rust 配置](../../rust/rust-toolchain.toml)、[Wasm Rust 版本](../../tools/wasm/rust-toolchain-version)

内部 registry 的首次填充仍需一台能够取得上游资料的同步机，之后开发者使用内部源。BCR 的 registry 元数据和真正的源码归档是两层地址，只改 `--registry` 不能保证所有下载都留在国内。官方 registry 的 `mirrors` 支持按原始 URL 路径保存源码包，默认仍有回退原始 URL 的语义；当前 seekdb wrapper 额外提供内部源限制和下载地址重写。[Bazel registry 规范](https://bazel.build/external/registry)、[本地 registry 脚本](../../tools/bazel_registry.py)、[本地 wrapper](../../bazel.py)

`tools/bazel_registry.py` 的归档预热仅处理传入 `--lockfile` 后，锁文件 `registryFileHashes` 中的 `source.json` 所指向的源码包。它不会自动收集模块扩展额外下载的工具、Cargo crate 或所有平台 SDK；wrapper 的 Bazel 下载器配置也不会自动约束 Cargo 等子进程的网络访问。因此它是镜像建设的一部分，不能直接当作全平台依赖已齐全的证明。目前确认的华为公共镜像覆盖 Bazel 程序本身，未确认有一个国内公共源完整覆盖 seekdb 所需的全部模块、工具链和 SDK。

已有的 `6.13.50.37` NativeLink 服务提供远程执行和构建结果缓存；它不会替开发者安装 Bazel、下载 Rust 或自动成为 BCR 源码镜像。工具分发、源码依赖镜像、构建缓存及远程执行各解决不同环节，可以逐步建设。

### 仅针对 Bazel 自身的无代理使用（已接入）

本轮只处理 Bazel 程序及其规则、模块的下载，Clang、Rust、Cargo 仍由现有环境提供。按用户要求，不将 rules_cc 等依赖随项目归档，不使用 vendor 或 distdir 绕过下载，也不要求搭建下载站。

入口统一为：

```bash
python3 bazel.py --no-proxy build --jobs=100 //src/observer:seekdb
```

新机器可先执行 `python3 bazel.py --no-proxy version`，单独触发 Bazel 安装并检查版本。Linux 默认安装目录为 `~/.cache/bazel/binaries/9.3.0/`，可由 `XDG_CACHE_HOME` 或 `--launcher-cache-dir` 改变，无需添加到 PATH。`python3 bazel.py --no-proxy deps init` 只准备 seekdb 的第三方依赖，不安装 Bazel；Rust/Cargo 及宿主机基础工具仍须预先准备。

- PATH 中没有 Bazel、且没有显式设置 `--bazel`/`BAZEL` 时，启动器会下载 `.bazelversion` 指定的 9.3.0。优先查询华为公共镜像，缺失时从 `releases.bazel.build` 获取；9.3.0 本次使用的是官方发布源。二进制 SHA256 固定在 [`bazel/binaries.json`](../../bazel/binaries.json)，覆盖 Linux/macOS/Windows 的 x86_64/ARM64 六种宿主。安装到用户缓存，无需 sudo、npm 或 Bazelisk。
- 依赖索引使用默认 `bcr.bazel.build`。[`bazel/downloader.cfg`](../../bazel/downloader.cfg) 将 GitHub 文件地址重写到 `mirror.bazel.build`，仅允许 BCR 和该镜像站；镜像缺包时明确失败，不悄悄回退 GitHub。现有显式 `--registry`/`--registry-mirror` 配置仍可覆盖默认路径。
- `--no-proxy` 仅清除本次启动器及其子进程的大小写 HTTP/HTTPS/ALL 代理变量，设置 `NO_PROXY=*`，不会修改 shell 或系统配置。Bazel 本体的自动下载始终优先节省代理流量，直接连接公开下载源。

这里的 BCR、依赖镜像和官方发布源均由 Bazel 官方托管在 Google Cloud，不能描述为中国境内镜像。[官方基础设施说明](https://blog.bazel.build/2026/01/16/ssl-cert-expiry.html) 本次验证的是当前网络环境可无代理获取完整 Bazel 构建依赖，不对其他运营商和所有地区的连通性作保证。

此前检查 27 个依赖的镜像地址，26 个返回 200，`rules_cc 0.2.22` 返回 404；这只是地址检查，并不是当时构建已启用镜像。进一步检查发现 `rules_cc 0.2.25` 有官方镜像，因此更新该版本。它依赖的 `apple_support 2.8.0` 在实际冷启动时返回 404，再将其选为镜像已有的 `2.8.4`；传递依赖中的 `bazel_skylib` 更新为 `1.9.0`。锁文件使用 Bazel 9.3.0 更新，其余版本仍按模块解析结果固定。rules_cc 0.2.25 默认不再配置旧 macOS 工具链，apple_support 会注册自己的 macOS 工具链；本次未做 macOS 编译测试。

验证先使用空启动器缓存、空 repository cache、新 output base、新 output_user_root，并禁用 repo contents cache。在 PATH 中移除现有 Bazel，将 HTTP/HTTPS/ALL 代理变量设为不可用的 `127.0.0.1:9`，通过 `--no-proxy` 执行目标分析。Bazel 9.3.0 从公开发布源重新完整下载并通过 SHA256 检查，依赖从 BCR 和官方源码镜像取得；两个目标均分析成功，共 16849 个配置目标、2445 次架构 aspect 应用。没有从旧 Bazel 依赖缓存复制文件。

下载器的本地 HTTP 测试覆盖镜像 404 后的回退、SHA256 错误时拒绝安装及已验证缓存的复用，三项通过。

随后再次创建另一套空 repository cache、新 output base 和 output_user_root，禁用 repo contents cache，恢复已有 C++/Rust 工具链环境，使用已通过上述冷启动安装验证的 Bazel 9.3.0 执行完整构建。保留严格 `--lockfile_mode=error`，经 `6.13.50.37:50051` 远程执行，`--jobs=100`；两个目标均成功，耗时 362.887 秒，1890 个动作中 374 个远程执行、329 个构建动作缓存命中、1187 个内部动作。构建动作缓存被复用，Bazel 依赖下载缓存没有复用；二者不能混淆。新的依赖缓存最终为 210 个文件、32651005 字节。

使用 `strace -f -e trace=connect` 跟踪启动器、Bazel、SSH 及构建子进程，没有出现本机代理端口 14869 或预设无效代理端口 9 的连接。记录包含 HTTPS、DNS、SSH 和远程执行隧道的本地端口。网络出口与其他地区网络不在本次验证范围内。

产物为 `/data/1/nijia.nj/build-cache/seekdb-bazel-direct-final.eZKeQZ/build/bin/src/observer/seekdb`，权限 0755，`--version` 返回 0，报告 seekdb 1.4.0.0、分支 bazel。构建日志为 `build_bazel/direct-clean-build.log`，连接记录为 `build_bazel/direct-build-network.log`，结构化证据为 `build_bazel/direct-validation.json`。本次没有部署数据库或运行 SQL 测试。

日常无代理远程构建入口（其他语言工具链和依赖按用户要求由现有环境提供）：

```bash
source ~/.bashrc
python3 bazel.py --no-proxy \
  --remote-executor=ssh://6.13.50.37:50051 \
  --remote-timeout=600s \
  build --jobs=100 \
  //src/observer:seekdb //src/observer:seekdb_source_ownership
```

首次构建不要设置 `CARGO_NET_OFFLINE=true`；上面的验证过程使用该设置，是因为本机已有完整 Cargo 缓存。


## seekdb 编译平台与 Bazel 覆盖

“Bazel 能在此宿主机运行”不等于“seekdb 的 Bazel 构建已经完整适配”。前者可由官方发行二进制和规则支持判断；后者取决于本仓库的平台约束、编译选项、依赖和链接规则。以下是静态分析结论。

| 平台/目标 | seekdb 现有构建范围 | Bazel 本身或生态能力 | 本仓库 Bazel 接入情况 |
| --- | --- | --- | --- |
| Linux x86_64 | `build.sh`/CMake；依赖初始化识别多种 Linux 发行版 | 8.2.1 与 9.3.0 均有官方宿主二进制 | 当前最明确的自有 C++ 工具链：execution/target 都限定 Linux x86_64 |
| Linux aarch64 | `build.sh`/CMake；依赖初始化支持的发行版子集见下文 | 有官方 `linux-arm64` 二进制；可配置 ARM64 C++ 工具链 | 当前自有工具链没有 ARM64 配置，不能推断为与 x86_64 同等完成 |
| macOS 13+ arm64 | Apple Silicon 为主要测试开发平台；macOS 27 有独立 Apple Clang/SDK 流程 | 有官方 `darwin-arm64` 二进制，可使用 Xcode/Command Line Tools | 有 macOS 分支及 `rules_cc` 原生宿主工具链回退；自有 repository 初始化仍检查 bundled LLVM，不能据此宣称所有新 macOS 流程完整可用 |
| macOS x86_64 | 检测脚本接受，但目前没有对应 x86_64 macOS deps 文件；不能等同开箱完整支持 | 有官方 `darwin-x86_64` 二进制 | Bazel 平台有能力；seekdb 依赖与仓库接入仍有缺口 |
| Windows 11 x64 | `build.ps1` 及独立 Windows 依赖流程 | 有官方 `windows-x86_64.exe`；C++ 工具链需要单独配置 | 有部分 Rust 平台分支，但 C++ 工具链及默认链接规则未呈现完整 Windows 适配 |
| Android arm64-v8a、API 28+ | 文档入口为 macOS + NDK 27.x，`build.sh --android` | `rules_android_ndk` 支持 Android NDK 25b+，有 ARM64 toolchain/platform | 未发现 Android 专用 Bazel 平台/规则接入；现有入口为 CMake |
| iOS arm64 真机与 arm64 模拟器 | Apple Silicon Mac + Xcode，`build.sh --ios [--simulator]`，输出 framework | `rules_apple`/Apple C++ 工具链支持 iOS，依赖 Xcode；需区分设备和模拟器平台 | 未发现对应 Bazel 平台/规则接入；现有入口为 CMake |
| WebAssembly | macOS/Linux + Emscripten 6.0.12，`build.sh --wasm` | Emscripten 官方 SDK 提供 Bazel toolchain 和 `wasm_cc_binary` | 未发现对应 Bazel 平台/规则接入；现有入口为 CMake |

Windows ARM64 不在本仓库已列出的 seekdb Windows 编译范围内，但 Bazel 8.2.1 和 9.3.0 均提供 `windows-arm64.exe`。Android、iOS、WebAssembly 在这里是交叉编译目标，不是运行 Bazel 的宿主平台。

本地依据：[工具链与宿主支持](../developer-guide/zh/toolchain.md)、[Android](../developer-guide/zh/android.md)、[iOS](../developer-guide/zh/ios.md)、[WebAssembly](../developer-guide/zh/webassembly.md)、[`MODULE.bazel`](../../MODULE.bazel)、[自有 C++ toolchain 模板](../../bazel/toolchains/cc/BUILD.seekdb_cc_toolchain.tpl)、[toolchain repository 初始化](../../bazel/toolchains/cc/seekdb_cc_toolchain_repo.bzl)。

外部依据：[Bazel 8.2.1 制品清单](https://github.com/bazelbuild/bazel/releases/expanded_assets/8.2.1)、[Bazel 9.3.0 制品清单](https://github.com/bazelbuild/bazel/releases/expanded_assets/9.3.0)、[macOS 安装](https://bazel.build/install/os-x)、[Windows 安装](https://bazel.build/install/windows)、[Android NDK rules](https://github.com/bazelbuild/rules_android_ndk)、[Apple rules](https://github.com/bazelbuild/rules_apple)、[Emscripten Bazel toolchain](https://github.com/emscripten-core/emsdk/blob/main/bazel/README.md)。

seekdb 依赖初始化识别的 Linux 发行版如下。这表示脚本有依赖配置分支，不表示所有版本、软件包、架构组合均经过本次验证或生产认证。[本地工具链文档](../developer-guide/zh/toolchain.md)

- **x86_64**：RHEL、CentOS、AlmaLinux、Rocky Linux、Alibaba Cloud Linux/AliOS、Anolis OS、TencentOS、Ubuntu、Debian、Fedora、Kylin、openEuler、openSUSE Leap、SLES、UOS。
- **aarch64**：RHEL、CentOS、AlmaLinux、Rocky Linux、Alibaba Cloud Linux/AliOS、Anolis OS、Ubuntu、Debian、Kylin、openEuler。

Bazel 的 Linux/CPU 支持不能直接替代上述发行版约束；旧发行版还要满足所选 Bazel 二进制和 C++ 工具链的系统运行库要求。当前未通过运行测试确定各发行版的具体兼容下限。

## 8.2.1 版本锁定的历史依据

检查 `.bazelversion`、`MODULE.bazel` 的 Git 历史，首次 Bazel 迁移提交 `4cb6fb268`（2026-08-07）已固定 8.2.1；公开 [PR #1258](https://github.com/oceanbase/seekdb/pull/1258) 未解释选版本的原因。当前找到的是版本限制本身，没有找到“曾因某个 Bazel 9 错误回退到 8.2.1”的提交或说明。对应 Dima 工作项尚未读取，用户随后将任务优先级调整为先完成 8.2.1 远程编译。

[Bazel 9 官方发布说明](https://github.com/bazelbuild/bazel/releases/tag/9.0.0)确实记录了 WORKSPACE 移除、C++ 原生规则迁入 rules_cc、默认关闭外部规则自动加载，以及 includes 从 `-isystem` 改为 `-I` 等变化。当前 seekdb 已使用 MODULE.bazel，并显式从 rules_cc 加载 C++ 规则、cc_common 和 CcInfo；rules_cc 0.2.22 自身的[测试矩阵](https://github.com/bazelbuild/rules_cc/blob/0.2.22/.bcr/presubmit.yml)也包含 Bazel 9。因此这些通用变化不能直接作为 seekdb 锁定版本的历史原因；升级兼容性需要另行验证。

## Linux x86_64 远程构建验证

在 `bazel` 分支、`a8afc89e9` 基线上修复 Bazel 声明后，使用现有 Bazel 8.2.1、LLVM 17.0.6、Rust 1.98.1 完成构建。C++ 编译经 SSH 隧道交给 `6.13.50.37:50051` 的现有 NativeLink 服务，设置 `--jobs=100`；构建期间确认远端实际执行编译任务。Rust 构建和最终链接由当前规则在本地执行。项目后续方向为彻底移除 CMake，本次修复面向 Bazel。

本机复用已有第三方依赖和 Cargo 缓存，没有重新下载大型工具链。`deps/3rd` 是指向同机 `rust-logger` 工作树依赖目录的本地链接，两份依赖清单一致；此链接未提交，其他机器仍须准备对应依赖。

本次构建命令：

```bash
source ~/.bashrc
./bazel.py \
  --remote-executor=ssh://6.13.50.37:50051 \
  --remote-timeout=600s \
  --disk-cache=/data/1/nijia.nj/build-cache/bazel-disk-cache \
  build --jobs=100 --keep_going \
  --action_env=CARGO_NET_OFFLINE=true \
  //src/observer:seekdb //src/observer:seekdb_source_ownership
```

修复包括：清理已删除的任务控制编译组和日志元数据头文件引用；补齐 Wasm I/O 与字符集头文件归属；修正配置模块对本地存储接口的依赖；将本地表生成头文件纳入 Share 编译依赖；补齐两个 Share Unity 编译组的 OBLib 依赖；使 jemalloc 头文件目标使用仓库统一静态库规则。

构建和 `seekdb_source_ownership` 检查均成功，产物为 `build_bazel/bin/src/observer/seekdb`。执行 `--version` 返回 0，报告 `seekdb 1.4.0.0`、分支 `bazel`、revision `1-a8afc89e9bb3fd4db41cefcb77274a1fe35879cc`。`git diff --check` 通过。本次没有部署数据库或运行 SQL 功能测试。

额外运行的独立静态检查仍有既有陈旧项：`share_header_ownership_check.py` 报告 `geo/geo_math.h`、`ob_tenant_role_transition_service.h` 缺少清单归属，以及 `resource/ob_server_resource.h` 分类需更新；`generate_oblib_header_inventory.py --check` 报告生成清单与当前源码不一致。此次仅修复编译所需的头文件声明，没有整体重分配 OBLib 公共接口归属。这些检查不应记为通过。

## 可写输出与 Bazel 9.3.0 实测

`.bazelrc` 已加入 `build --experimental_writable_outputs`。8.2.1 重新构建成功，现有二进制权限从 `0555` 变为 `0755`；9.3.0 的产物也为 `0755`。此选项允许文件所有者修改构建输出，后续构建仍可能替换手工修改的内容。

2026-10-16 再次查询[官方最新稳定版](https://github.com/bazelbuild/bazel/releases/latest)，结果为 9.3.0。测试二进制保存在本机 `/data/1/nijia.nj/build-cache/bazel-binaries/bazel-9.3.0-linux-x86_64`，没有替换 PATH 中的 8.2.1。下载先尝试直连，速度较慢后经 `cx` 续传；SHA256 与[官方校验文件](https://releases.bazel.build/9.3.0/release/bazel-9.3.0-linux-x86_64.sha256)一致：`d302d22ed77ee5658b87857249baa579db84300ecebe0a1e9a357b5b20f80eb8`。

测试期间暂将 `.bazelversion` 设为 `9.3.0`、`MODULE.bazel` 的版本上限设为 `<=9.3.0`，使用独立输出目录 `build_bazel/bazel9-check`。在前述 8.2.1 构建修复的基础上，得到以下结果：

| 阶段 | 实测结果 | 继续验证的方法 |
| --- | --- | --- |
| 默认锁文件策略 | 在计算仓库映射时失败：`The version of MODULE.bazel.lock is not supported by this version of Bazel` | 临时使用 `--lockfile_mode=update`，锁文件格式从 18 升为 28；后续构建恢复 `--lockfile_mode=error` 并通过 |
| 默认执行环境 | Rust genrule 失败：`FileNotFoundError: [Errno 2] No such file or directory: 'cargo'`；动作中的 PATH 仅有 `/bin:/usr/bin:/usr/local/bin` | 测试命令增加 `--action_env=PATH`，显式传递已加载工具链的 PATH |
| 加入 PATH 后 | Rust、C/C++ 编译、最终链接和 `seekdb_source_ownership` 均成功 | 未再修改业务源码或 C++ 构建规则 |

Bazel 9 默认启用 `--incompatible_strict_action_env`，而当前 `rust/build.py` 直接执行 PATH 中的 `cargo`，因此可复现上述环境依赖问题。[Bazel 9 发布说明](https://github.com/bazelbuild/bazel/releases/tag/9.0.0)也说明它最低依赖 protobuf 33.4，本次更新相应下载了规则依赖。这里的结果说明当前代码可以使用 9.3.0 编译，不能证明最初固定 8.2.1 的历史原因。

成功构建命令（要求先临时调整上述版本限制，并完成锁文件更新）：

```bash
source ~/.bashrc
cx ./bazel.py \
  --bazel=/data/1/nijia.nj/build-cache/bazel-binaries/bazel-9.3.0-linux-x86_64 \
  --build-dir=build_bazel/bazel9-check \
  --remote-executor=ssh://6.13.50.37:50051 \
  --remote-timeout=600s \
  --disk-cache=/data/1/nijia.nj/build-cache/bazel-disk-cache \
  --repository-cache=/home/nijia.nj/.cache/bazel/repository-cache \
  build --jobs=100 --keep_going --lockfile_mode=error \
  --action_env=PATH --action_env=CARGO_NET_OFFLINE=true \
  //src/observer:seekdb //src/observer:seekdb_source_ownership
```

最终一次构建耗时约 186 秒，记录 405 个远程动作。产物为 `build_bazel/bazel9-check/bin/src/observer/seekdb`；执行 `--version` 返回 0，报告 seekdb 1.4.0.0、分支 bazel、同一源码 revision。此结论仅覆盖本机 Linux x86_64 构建和版本输出，没有运行 SQL 功能测试或跨平台构建。

另在依赖加载缓慢时取消过一次构建，该次取消触发 Bazel 远程连接清理的 `ReferenceCountedChannel` / `InterruptedException` / `AssertionError`，客户端退出码为 37；随后再次取消返回正常中断码 8，完整构建返回 0。尚未确定该清理异常的稳定触发条件，不将它归因于 seekdb 源码。

原始日志保留在 `build_bazel/bazel9-check/01-default.log`、`02-update-lock.log`、`03-direct-network.log`、`04-proxy-network.log`、`05-explicit-path.log`。测试用版本文件和更新后的锁文件保存在同目录的 `test-config/` 下。首次试验后曾恢复 8.2.1；随后按用户要求正式采用 9.3.0，当前状态如下。

`.bazelversion` 和 `MODULE.bazel` 均固定为 9.3.0，版本 28 的 `MODULE.bazel.lock` 已纳入仓库改动，`.bazelrc` 固定 `--action_env=PATH` 和可写输出选项。本机 `bazel` 入口已切到 `/data/1/nijia.nj/tools/bazel/9.3.0/bazel-9.3.0-linux-x86_64`。使用普通 `./bazel.py` 入口和默认 `build_bazel` 输出目录重新构建成功，无需临时版本文件或额外 PATH 参数；复用了上次 9.3.0 构建的磁盘缓存。`build_bazel/bin/src/observer/seekdb` 权限为 `0755`，`--version` 返回 0。最终日志为 `build_bazel/build-9.3.0.log`。
