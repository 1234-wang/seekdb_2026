# 获取代码、编译并运行 seekdb

## 前置条件

先按照[安装工具链](toolchain.md)安装支持的编译器和依赖。完整源码编译需要较多的磁盘空间和内存。

## 获取代码

```bash
git clone https://github.com/oceanbase/seekdb.git
cd seekdb
```

## 编译生产二进制

仓库的 `build.py` 下载 `.bazelversion` 指定版本的 Bazel，并执行构建命令。默认使用 `-O2` 优化，保留调试信息。

```bash
./build.py init
./build.py build seekdb
```

`init` 准备平台依赖，首次编译或依赖定义变化后执行。后续增量编译只需重复 `build` 命令。产物位于 `build/bin/src/observer/seekdb`。

Windows 使用 `python build.py`，命令参数相同。C++ 测试目标参见[编写与运行单元测试](unittest.md)。修改 Rust 依赖时，一并提交更新后的 `rust/Cargo.lock`。

## 设置编译并发

Bazel 默认按本机 CPU 数量设置并发。Bazel 参数放在命令之后：

```bash
./build.py build --jobs=16 seekdb
```

## 使用远程 C++ 编译

指定 REAPI 服务地址和并发数：

```bash
./build.py build --config=remote --remote_executor=grpc://HOST:PORT --jobs=100 seekdb
```

C++ 编译在远端执行。Cargo、生成器和链接仍在本机运行。如果服务需要 SSH 隧道，先单独建立隧道。

## 编译 Sanity 二进制

Linux 上通过以下命令启用 Sanity 插桩：

```bash
./build.py build --sanity=true seekdb
```

产物仍通过相同的链接访问。如需保留独立的 Sanity 构建，为它指定独立的 Bazel 工作目录和产物链接：

```bash
./build.py --build-dir build_sanity --output_base="$HOME/.cache/seekdb-sanity" build --sanity=true seekdb
```

二进制通过 `build_sanity/bin/src/observer/seekdb` 访问。

## 构建目录

Bazel 将实际工作目录放在源码树外的用户缓存目录中，每份源码有独立的目录。使用 `./build.py info output_base` 查看实际位置。项目内的 `build/bin`、`build/out` 等是指向它的链接；Linux/macOS 的二进制仍通过 `build/bin/src/observer/seekdb` 访问。

`--build-dir PATH` 只改变这些链接的位置。需要隔离实际构建状态时，在命令前指定 Bazel 的 `--output_base=PATH`。普通 `find` 不遍历目录软链接，`find -L` 会遍历。

使用 `./build.py clean --expunge` 清理当前源码的工作目录。旧版入口留下的 `build/output-base` 不会自动迁移，可用 `./build.py --output_base="$PWD/build/output-base" clean --expunge` 清理。

## 复用构建缓存

Bazel 二进制和默认的构建结果缓存可供不同代码目录共用。它们的根目录在 Linux 上是 `~/.cache/bazel`，macOS 上是 `~/Library/Caches/bazel`，Windows 上是 `%LOCALAPPDATA%/bazel`。Linux 支持 `XDG_CACHE_HOME`。依赖下载缓存由 Bazel 单独管理，也可跨代码目录复用。

指定 Bazel 二进制缓存目录时，将 `--launcher-cache-dir PATH` 放在命令前。指定构建结果或依赖下载缓存目录时，将 `--disk_cache=PATH` 或 `--repository_cache=PATH` 放在命令后。例如：

```bash
./build.py --launcher-cache-dir /data/bazel build --disk_cache=/data/bazel-actions seekdb
```

如需更改依赖下载设置，在命令后使用 Bazel 的 `--registry=URL` 或 `--downloader_config=PATH`。

## 为其他平台编译

各平台统一使用 `seekdb` 目标，通过 `--config=android`、`--config=ios`、`--config=ios-simulator` 或 `--config=wasm` 选择平台。不指定时构建本机程序。依赖准备方式见 [Android](android.md)、[iOS](ios.md) 和 [WebAssembly](webassembly.md) 指南。Android 生成服务端程序，iOS 生成 framework，WebAssembly 生成引擎和 SQL shell；别名不改变各平台的产物路径。

## 维护构建声明

根据需要修改的内容选择文件：

| 文件 | 职责 |
| --- | --- |
| `build.py` | 启动指定版本的 Bazel，通过 `init` 准备平台依赖。 |
| `.bazelrc` | Bazel 默认参数和各平台的命名配置。 |
| `MODULE.bazel` | Bazel 模块依赖，以及仓库和工具链注册。 |
| `bazel/defs.bzl` | 供 `BUILD.bazel` 使用的公共 C++ 和链接规则入口。 |
| `bazel/compile_config.bzl` | 公共编译参数和 Sanity 插桩配置。 |
| `bazel/build_settings.bzl`、`bazel/BUILD.bazel` | 自定义配置项类型和公共开关声明。 |
| 各目录的 `BUILD.bazel` | 目标、头文件、显式依赖和可见性。 |

C++ 库从 `//bazel:defs.bzl` 加载 `cc_library`、`unity_cc_library` 或 `grouped_unity_cc_library`。`cc_library` 分别编译每个源文件；`unity_cc_library` 将有序的 `unity_members` 列表合为一个编译单元；`grouped_unity_cc_library` 为 `unity_groups` 中的每个命名分组生成一个编译单元。

- 普通源码加入所属目标的 `srcs`。加入已有 Unity 分组时，在该模块 `*_source_inventory.bzl` 对应分组的 `srcs` 中添加仓库相对路径，保留成员顺序。使用 `unity_members` 的目标直接在 `BUILD.bazel` 中列出包内相对路径。
- 不应与其他源码合编的文件，加入所属目标使用的独立源码列表。列表直接填写路径，C 和 C++ 分开，由规则调用指定语言。新增列表还需要在 `BUILD.bazel` 中接入目标。
- 公共头文件加入 `hdrs` 或模块的公共头文件列表；私有头文件放入 `srcs` 或模块内部头文件目标。使用头文件 `glob()` 的模块会自动纳入匹配的新文件。Unity 辅助规则对应的属性名是 `public_hdrs` 和 `private_hdrs`。
- 公共头文件需要的依赖放入 `deps`，只有实现文件需要的依赖放入 `implementation_deps`。用 `visibility` 限定使用方。依赖由开发者显式维护，没有扫描 include 并自动生成依赖的脚本。

通过 `./build.py build //package:target` 编译受影响的目标。现有模块依赖方向和头文件分层检查随构建执行。

## 运行本地实例

使用仓库中的 `obd.sh` 包装脚本准备隔离环境并启动 seekdb：

```bash
./tools/deploy/obd.sh prepare -p /tmp/obtest
./tools/deploy/obd.sh deploy -c ./tools/deploy/single.yaml
```

从 `tools/deploy/single.yaml` 读取 `mysql_port`。如果生成的端口是 `10000`，可以使用以下任一客户端连接：

```bash
mysql -h127.0.0.1 -P10000 -uroot
./deps/3rd/u01/obclient/bin/obclient -h127.0.0.1 -P10000 -uroot -Doceanbase -A
```

如果生成了其他端口，应以文件中的实际值为准。

## 停止并清理本地实例

```bash
./tools/deploy/obd.sh destroy --rm -n single
```

该命令会停止实例并删除此示例创建的部署数据。不要对需要保留的数据执行该命令。
