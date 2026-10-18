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
./build.py build //src/observer:seekdb
```

`init` 准备平台依赖，首次编译或依赖定义变化后执行。后续增量编译只需重复 `build` 命令。产物位于 `build_bazel/bin/src/observer/seekdb`。

Windows 使用 `python build.py`，命令参数相同。C++ 测试目标参见[编写与运行单元测试](unittest.md)。修改 Rust 依赖时，一并提交更新后的 `rust/Cargo.lock`。

## 设置编译并发

Bazel 默认按本机 CPU 数量设置并发。Bazel 参数放在命令之后：

```bash
./build.py build --jobs=16 //src/observer:seekdb
```

## 使用远程 C++ 编译

指定 REAPI 服务地址和并发数：

```bash
./build.py build --config=remote --remote_executor=grpc://HOST:PORT --jobs=100 //src/observer:seekdb
```

C++ 编译在远端执行。Cargo、生成器和链接仍在本机运行。如果服务需要 SSH 隧道，先单独建立隧道。

## 编译 Sanity 二进制

Linux 上通过以下命令启用 Sanity 插桩：

```bash
./build.py build --sanity=true //src/observer:seekdb
```

产物路径与普通构建相同。如需保留独立的 Sanity 构建目录，在 `build` 前加 `--build-dir build_sanity`，产物将位于 `build_sanity/bin/src/observer/seekdb`。

## 复用构建缓存

Bazel 二进制和默认的构建结果缓存可供不同代码目录共用。它们的根目录在 Linux 上是 `~/.cache/bazel`，macOS 上是 `~/Library/Caches/bazel`，Windows 上是 `%LOCALAPPDATA%/bazel`。Linux 支持 `XDG_CACHE_HOME`。依赖下载缓存由 Bazel 单独管理，也可跨代码目录复用。

指定 Bazel 二进制缓存目录时，将 `--launcher-cache-dir PATH` 放在命令前。指定构建结果或依赖下载缓存目录时，将 `--disk_cache=PATH` 或 `--repository_cache=PATH` 放在命令后。例如：

```bash
./build.py --launcher-cache-dir /data/bazel build --disk_cache=/data/bazel-actions //src/observer:seekdb
```

如需更改依赖下载设置，在命令后使用 Bazel 的 `--registry=URL` 或 `--downloader_config=PATH`。

## 为其他平台编译

按 [Android](android.md)、[iOS](ios.md) 或 [WebAssembly](webassembly.md) 指南准备依赖并选择目标。Android 使用上面的服务端目标，iOS 生成 framework，WebAssembly 生成引擎和 SQL shell。

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
