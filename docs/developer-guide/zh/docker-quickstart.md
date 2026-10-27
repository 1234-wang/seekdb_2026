# 使用 Docker 快速体验 seekdb

本文介绍如何用官方镜像在几分钟内启动一个 seekdb 实例，无需编译源码。适合快速体验功能、验证 SQL 兼容性或搭建本地测试环境。

## 前提条件

* 已安装 Docker（19.03 及以上版本）。
* 宿主机至少 1 核 CPU、2 GB 可用内存。

## 启动实例

```bash
docker run -d --name seekdb -p 2881:2881 oceanbase/seekdb
```

实例启动后会监听 2881 端口。首次启动需要初始化数据目录，通常需要数十秒，可通过日志确认状态：

```bash
docker logs -f seekdb
```

当日志中出现 `SeekDB SQL ready` 时，表示实例已可接受连接。

## 连接实例

```bash
docker exec -it seekdb mysql -h127.0.0.1 -P2881 -uroot -A oceanbase
```

也可以从宿主机直接连接映射端口：

```bash
mysql -h127.0.0.1 -P2881 -uroot -A oceanbase
```

root 用户默认密码为空。如需设置密码，启动时通过环境变量传入：

```bash
docker run -d --name seekdb -p 2881:2881 -e ROOT_PASSWORD='your_password' oceanbase/seekdb
```

## 配置文件与数据目录

| 路径 | 说明 |
| --- | --- |
| `/etc/seekdb/seekdb.cnf` | 实例配置文件，可在启动时挂载自定义文件覆盖 |
| `/var/lib/seekdb` | 数据目录（1.4 及以上版本）；1.3.x 版本为 `/var/lib/oceanbase` |
| `/var/lib/seekdb/run/telemetry.json` | 遥测数据的本地副本，记录实例标识与运行环境信息 |

挂载自定义配置文件：

```bash
docker run -d --name seekdb -p 2881:2881 \
  -v /path/to/seekdb.cnf:/etc/seekdb/seekdb.cnf \
  oceanbase/seekdb
```

> **注意**
>
> 使用自定义配置文件时，不要同时指定 `CPU_COUNT`、`MEMORY_LIMIT`、`LOG_DISK_SIZE` 等资源类环境变量，否则配置可能冲突。

## 数据持久化

容器内的数据默认不会保留到宿主机。需要持久化时，挂载一个数据卷到数据目录：

```bash
docker volume create seekdb_data
docker run -d --name seekdb -p 2881:2881 -v seekdb_data:/var/lib/seekdb oceanbase/seekdb
```

> **注意**
>
> 在 Windows 上运行容器时，请使用 Docker volume 而不是宿主机目录挂载，以确保数据目录正常工作。

## 关闭与清理

```bash
docker stop seekdb      # 停止实例，容器与数据保留
docker start seekdb     # 重新启动
docker rm -f seekdb     # 删除容器（未挂载卷时数据会一并丢失）
```

## 遥测说明

seekdb 内置遥测功能并默认开启，仅采集匿名化的版本、部署方式与主机环境信息。关闭方式：

```bash
docker run -d --name seekdb -p 2881:2881 -e TELEMETRY_ENABLED=false oceanbase/seekdb
```
