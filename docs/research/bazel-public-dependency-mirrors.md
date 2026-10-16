# Bazel 公共依赖镜像调查

调查日期：2026-10-17。调查基线：`6b5fc35bf`，Bazel 9.3.0。目标是在开发者不配置本地代理、不自建下载服务、不把 rules_cc 等依赖归档进项目的前提下，寻找能覆盖当前依赖的公共下载路径。调查完成后，项目默认依赖下载配置已采用下文验证过的 GH-Proxy 单一来源；Bazel 可执行文件由 `bazel/binaries.json` 单独配置，也已改为 GH-Proxy，六个平台的完整下载验证见[工具链调查](bazel-toolchain-platforms.md#gh-proxy-六平台二进制验证)。

## 当前得到的候选

本次找到了可用的公共路径：**GH-Proxy `.com` 覆盖了当前项目的 191 个锁定元数据文件、20 个补丁以及 27 个模块源码归档，全部通过原有哈希校验**；在用户 Mac 的完整下载抽测中，它比 Cloudflare 归档镜像更快。元数据和补丁在本机检查，27 个归档在用户 Mac 检查；另外在本机用真正的 Bazel 9.3.0、隔离缓存、原锁文件严格模式，完成了三个直接模块的 fetch。无需自建服务，也无需开发者安装代理客户端。

Cloudflare 也已有专门面向 BCR 源码归档的公共镜像，BCR 维护者确认过这一服务；它在用户 Mac 的 27 个归档下载同样全部通过哈希校验。它可以作为另一个供选择的渠道，**不是**项目自动 fallback。

这不等于找到了“保证全国各网络长期稳定”的镜像。下面的成功数只代表 2026-10-17 当前项目指定文件的检查结果，不把单次 HTTP 200 当作完整构建通过，也不把只读文件的哈希验证当作所有平台构建验证。

| 渠道 | 定位 | 当前验证结论 |
| --- | --- | --- |
| `gh-proxy.com` | 公共 GitHub Raw / Release / Archive 反向代理；使用官方 BCR 和上游发布内容 | 本机无代理：**191/191** 元数据、**20/20** 补丁校验一致；用户 Mac 无代理：**27/27** 归档完整下载并校验一致，共 45,809,205 B |
| `bcr.cloudflaremirrors.com` | Cloudflare 提供的 BCR 源码归档镜像 | 用户 Mac 无代理完整下载 **27/27** 个当前模块归档，全部 SHA256 一致，共 45,809,205 B；不包含 BCR 元数据 |
| `cdn.jsdelivr.net/gh/bazelbuild/bazel-central-registry@main/` | 通用 GitHub 文件 CDN | 27 个 source.json 加根配置仅 **18/28** 成功；部分请求触发包大小限制，不能直接用于当前完整 BCR |
| `testingcf.jsdelivr.net/gh/bazelbuild/bazel-central-registry@main/` | 同一 jsDelivr 服务的另一入口 | 锁文件 **162/191** SHA256 一致，剩余 **29/191** 返回 403，原因均为 BCR 仓库超过其配置的 50 MB 限制 |

GH-Proxy 运营者文档明确支持 Raw、Release、Archive 下载；它属于第三方反向代理，不是 Bazel 官方维护的国内 registry。[GH-Proxy 使用说明](https://gh-proxy.com/docs/github-accelerator)

Cloudflare 归档镜像由 BCR 维护者在官方仓库讨论中确认。BuildBuddy 自己的公共配置也使用 `--module_mirrors=https://bcr.cloudflaremirrors.com`。[BCR 维护者说明](https://github.com/bazelbuild/bazel-central-registry/discussions/7921)、[BuildBuddy 配置](https://github.com/buildbuddy-io/buildbuddy/blob/master/shared.bazelrc)

## 哪些内容需要一起覆盖

官方 BCR 的内容来自 `bazelbuild/bazel-central-registry`。Bazel 读取 registry 的 `MODULE.bazel`、`source.json`、根配置以及补丁；`source.json` 再指向实际源码归档。Registry 本身无需托管归档。因此换 registry 地址和换归档镜像是两件可以独立处理的事。[Bazel registry 规范](https://bazel.build/external/registry)、[BCR 官方说明](https://github.com/bazelbuild/bazel-central-registry)

当前 [`MODULE.bazel.lock`](../../MODULE.bazel.lock) 包含 **191** 个 registry 文件 SHA256，其中有 **27** 个 `source.json`。这 27 个源文件的归档 URL 全在 `github.com`，并另外引用 **20** 个 registry 补丁。验证完整性时使用的是现有锁文件和其中 source.json 指定的 integrity，没有采用镜像返回的新校验值。

这里的 27 个归档是当前解析出的模块源码包，**不代表**任意平台下模块扩展会下载的全部 JDK、SDK、工具制品，也不代表新版本依赖永远可用。新平台、新依赖或模块扩展的额外下载还需要单独检查。

## 元数据与补丁实测

本机为 `6.12.232.130`。Python 请求显式使用 `ProxyHandler({})`，只禁用本次进程的代理，未修改系统代理配置。最多 8 个并发请求，每个请求 15 秒超时，逐个文件完整读取后校验哈希，没有失败重试或下载回退。时间是单文件请求耗时，不是整个 Bazel 构建耗时。

| 请求内容 | 渠道 | 通过数 | 下载总量 | 单文件耗时中位数 | 最长耗时 |
| --- | --- | --- | ---: | ---: | ---: |
| 锁文件全部 registry 元数据 | GH-Proxy `.com` Raw | 191/191 | 288,108 B | 0.525 秒 | 1.121 秒 |
| 27 个 source.json 中的全部补丁 | GH-Proxy `.com` Raw | 20/20 | 12,571 B | 0.786 秒 | 1.338 秒 |
| 锁文件全部 registry 元数据 | jsDelivr `testingcf` | 162/191 | 成功内容 253,949 B | 1.788 秒 | 8.404 秒 |
| 27 个 source.json 和根配置 | jsDelivr `cdn` | 18/28 | 成功内容 5,658 B | 2.322 秒 | 4.596 秒 |

GH-Proxy 可用文件示例：

```text
https://gh-proxy.com/https://raw.githubusercontent.com/bazelbuild/bazel-central-registry/main/modules/rules_cc/0.2.25/source.json
https://gh-proxy.com/https://raw.githubusercontent.com/bazelbuild/bazel-central-registry/main/modules/rules_cc/0.2.25/patches/module_dot_bazel_version.patch
```

另对同一个 rules_cc source.json 做单文件抽测：`gh-proxy.org` 返回正确内容、1.061 秒；`ghproxy.net` 返回正确内容、1.742 秒；直连 `raw.githubusercontent.com` 当次也返回正确内容、0.817 秒。这些仅是单个文件的抽测，没有把它们记作完整覆盖。GH-Proxy 当前文档的示例主要推荐 `.org`，与 `.com` 的测速结果应分开记录。[运营者文档](https://gh-proxy.com/docs/github-accelerator)

jsDelivr 的典型失败正文为：

```text
Package size exceeded the configured limit of 50 MB.
```

有些文件仍成功说明某些内容可从该 CDN 取得，但整套检查已经证明不能仅凭一个 source.json 成功就配置全项目。其官方说明也有包和单文件大小限制，并说明分支缓存通常持续 12 小时。这里记录的 50 MB 是服务当前返回的实际限制；官网总说明中的默认限制不代表 BCR 仓库在每个入口均已被放行。[jsDelivr GitHub 用法、限制与缓存说明](https://github.com/jsdelivr/jsdelivr)

## Cloudflare 归档路径与 User-Agent

归档路径格式是镜像根 URL 加原 URL 去掉协议后的部分，例如：

```text
https://bcr.cloudflaremirrors.com/github.com/bazelbuild/rules_cc/releases/download/0.2.25/rules_cc-0.2.25.tar.gz
```

Bazel 9.3.0 的 `IndexRegistry.createArchiveRepoSpec` 按 mirror、原 URL authority、path 拼接该地址。该机制只处理模块源码归档，registry 文件和补丁仍从 registry 读取。[Bazel 9.3.0 实现](https://github.com/bazelbuild/bazel/blob/9.3.0/src/main/java/com/google/devtools/build/lib/bazel/bzlmod/IndexRegistry.java)

本机初次使用 Python 默认 `Python-urllib/3.8` 请求这个归档返回 HTTP 403，正文 `error code: 1010`。保持地址和网络相同，只换为 `User-Agent: Bazel/9.3.0` 后，Range GET 返回 206、`application/gzip` 和正确 gzip 文件头，耗时 0.666 秒；`curl/8.7.1` 和浏览器 User-Agent 同样成功。因此不能把初次 403 直接解释为文件缺失或中国地域封锁。Cloudflare 官方把 1010 解释为按客户端签名阻止访问。[Cloudflare 1010 文档](https://developers.cloudflare.com/support/troubleshooting/http-status-codes/cloudflare-1xxx-errors/error-1010/)

进一步核对 Bazel 9.3.0 代码，原生下载器实际使用小写 `bazel/9.3.0`。使用这个精确 User-Agent 重测，仍返回 206 和正确 gzip 文件头，耗时 2.022 秒。因此这里是探测工具默认 User-Agent 引起的误判，不需要给项目启动器增加 User-Agent 配置。[Bazel HttpConnector 实现](https://github.com/bazelbuild/bazel/blob/9.3.0/src/main/java/com/google/devtools/build/lib/bazel/repository/downloader/HttpConnector.java)

使用 Bazel User-Agent 请求该镜像的 `/bazel_registry.json` 和 `/modules/rules_cc/0.2.25/source.json` 均返回 404。这也支持将它视为归档镜像，而非直接替换 `--registry` 的元数据服务。

## 用户 Mac 的归档实测

在用户实际遇到下载缓慢的 Mac `30.174.76.160` 上，显式禁用本次测试进程的代理，完整下载文件并与已验证的 BCR `source.json` 中 integrity 对照。Cloudflare 的 27 个模块归档以最多 4 并发全部完成，**27/27 校验通过，总量 45,809,205 B**。没有修改 Mac 项目的下载配置，也没有把这些包归档进 seekdb。

GH-Proxy `.com` 随后对 protobuf、rules_cc、rules_kotlin 三个文件串行完整下载，并补齐其余 24 个模块归档，**总计 27/27 全部 SHA256 一致，同样为 45,809,205 B**。为减少并发差异，另将 Cloudflare 的 protobuf 单文件串行重测一次。

| 文件 | 大小 | Cloudflare，最初 4 并发批次 | GH-Proxy `.com`，串行 | Cloudflare，另一次串行 |
| --- | ---: | ---: | ---: | ---: |
| `protobuf/33.4` | 6,889,704 B | 24.825 秒 | 5.282 秒 | 27.002 秒 |
| `rules_cc/0.2.25` | 524,157 B | 1.640 秒 | 1.295 秒 | 未重测 |
| `rules_kotlin/1.9.6` | 23,089,629 B | 62.641 秒 | 26.863 秒 | 未重测 |

这些数值是当次完整 HTTP 下载耗时，包含连接和文件传输，不含 Bazel 解压、补丁和构建。不同批次时间、并发条件不同，不能作为固定提速倍数，也不能直接与此前 Bazel profile 中包含解压的 fetch 时间作同口径比较。protobuf 的同为串行结果支持 GH-Proxy 在该机器当时较快，但不构成长期带宽保证。

## 没有确认可用的国内完整 BCR 服务

本次检索大学镜像、华为云、Gitee/GitCode，未找到运营方明确维护且能通过当前文件验证的完整国内 BCR 服务。以下是**具体路径的失败证据**，并非声称这些运营方永远没有其他服务：

| 检查地址前缀，后接 `/modules/rules_cc/0.2.25/source.json` | 实测结果 |
| --- | --- |
| `https://mirrors.huaweicloud.com/bcr` | HTTP 200，但为 12,434 B HTML 页面，SHA256 不匹配 |
| `https://mirrors.nju.edu.cn/bazel-central-registry` | HTTP 404 |
| `https://mirrors.sjtug.sjtu.edu.cn/bazel-central-registry` | HTTP 404 |
| `https://gitee.com/mirrors/bazel-central-registry/raw/main` | HTTP 404 |
| `https://gitcode.com/gh_mirrors/ba/bazel-central-registry/raw/main` | HTTP 200，但为 5,792 B HTML 页面，SHA256 不匹配 |
| `https://ghfast.top/https://raw.githubusercontent.com/bazelbuild/bazel-central-registry/main` | HTTP 403 |
| `https://raw.gitmirror.com/bazelbuild/bazel-central-registry/main` | DNS 名称解析失败 |

华为云的 Bazel 可执行文件目录不能据此当作 rules_cc、protobuf 等依赖镜像；GitCode/Gitee 的仓库网页也不能据此当作满足 Bazel index registry 格式的下载端点。

## 配置落地时的约束

如果选择公共渠道，只配置选中的单一来源，保留现有 SHA256/integrity 校验，不增加多个下载源回退，也不归档第三方依赖。单独添加 `--module_mirrors` 会把镜像放在原 URL 之前，Bazel 仍可能尝试原 URL；要满足本项目“没有 fallback”的要求，需要同时使用 downloader allow/block/rewrite 约束实际下载路径。[Bazel 9.3.0 镜像 URL 构造代码](https://github.com/bazelbuild/bazel/blob/9.3.0/src/main/java/com/google/devtools/build/lib/bazel/bzlmod/IndexRegistry.java)

此外，项目启用了 `--lockfile_mode=error`，锁文件中的 registry 哈希按原 URL 索引。若直接更换 `--registry`，需要一并处理锁文件 URL 与校验，不能假设换一个地址后现有锁就自动适配。[Bazel 9.3.0 registry 锁文件校验代码](https://github.com/bazelbuild/bazel/blob/9.3.0/src/main/java/com/google/devtools/build/lib/bazel/bzlmod/IndexRegistry.java)

另一条路径是保持逻辑 registry 地址，仅让 downloader 重写传输地址。Bazel 9.3.0 的 `DownloadManager.downloadAndReadOneUrlForBzlmod` 确实先对原 URL 应用 downloader rewrite，再携带原有 checksum 下载。因而可以保留现有锁文件的官方 URL 和哈希；实际生效与完整下载仍应通过隔离 fetch 确认。[Bazel 9.3.0 DownloadManager 实现](https://github.com/bazelbuild/bazel/blob/9.3.0/src/main/java/com/google/devtools/build/lib/bazel/repository/downloader/DownloadManager.java)

## 隔离 Bazel fetch 验证

本机 `6.12.232.130` 使用 Bazel 9.3.0、临时工作区和独立缓存，显式禁用测试进程代理。测试工作区只声明以下三个直接模块，没有编译 seekdb：

```starlark
module(name = "mirror_probe")
bazel_dep(name = "rules_cc", version = "0.2.25")
bazel_dep(name = "apple_support", version = "2.8.4")
bazel_dep(name = "platforms", version = "1.0.0")
```

将项目现有 `MODULE.bazel.lock` 复制到该临时工作区，启用 `--lockfile_mode=error`，只允许 GH-Proxy 下载。Bazel 输出 `All requested repos fetched successfully.`，退出码为 0，耗时 11.843 秒；测试前后锁文件字节未变。实际使用的 downloader 配置如下：

```text
rewrite bcr\.bazel\.build/(.*) https://gh-proxy.com/https://raw.githubusercontent.com/bazelbuild/bazel-central-registry/main/$1
rewrite (?:mirror\.bazel\.build/)?github\.com/(.*) https://gh-proxy.com/https://github.com/$1
allow gh-proxy.com
block *
```

这同时覆盖官方 BCR 元数据、GitHub 原归档 URL、原配置中 Google 镜像的 GitHub 归档 URL。只允许 `gh-proxy.com`，因此成功不是因为下载失败后改走 GitHub 或 Google。逻辑 registry 仍为 `bcr.bazel.build`，现有锁文件哈希继续生效。

另一个隔离工作区使用同样三个模块，元数据走 GH-Proxy、归档走 Cloudflare，`--lockfile_mode=off` 下 fetch 也成功。对应配置只把上面的第二条 rewrite 目标改为 `https://bcr.cloudflaremirrors.com/github.com/$1`，并增加 `allow bcr.cloudflaremirrors.com`。这项只验证下载器与归档镜像能配合工作，严格锁文件验证证据来自前一个 GH-Proxy 测试。

上述 fetch 是**三个直接模块的隔离集成验证**，并非当前 seekdb 全部模块的 Bazel fetch，也非 Linux/macOS/Windows 全平台编译验证。27 个归档完整性检查、191 个元数据检查和这个集成验证分别回答覆盖范围、文件正确性及 Bazel 配置能否生效的问题。

项目的 [`bazel/downloader.cfg`](../../bazel/downloader.cfg) 已采用上述 GH-Proxy 单一来源配置，现有 `MODULE.bazel.lock` 保持不变。临时 Bazel 安装解压目录和构建缓存已清理，完整下载记录、测试配置与日志保留在调查机器的 `/data/1/tmp/seekdb-public-mirror-research-bgynlxhf/`，其中 `mac-full-downloads.jsonl` 与 `mac-full-downloads-extra.jsonl` 是 Mac 的逐文件记录，`gh-fetch-check/downloader.cfg` 与 `gh-fetch-check/fetch.log` 是严格锁文件模式的配置和结果。
