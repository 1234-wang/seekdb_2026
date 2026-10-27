> Draft evidence scope: the measured product source is integrated commit `5ee7271c6f750894a3e1ff15c9d6b2afa5f44ff9`. The two AMD product translation units and helper code in this minimal official-baseline branch match that measured implementation exactly. This standalone branch has **not** yet been fully compiled or run. The new portable test wrapper has only parse/PLAN/path/digest validation; the original complete SQL test flow is AST-identical after removing the documented wrapper adaptation. No official performance target or score is claimed.

# AMD hipVS 原生索引构建实验报告

本次在同一台实际 AMD 云端机器、同一个 seekdb 二进制上，完成 CPU 与 GPU 各三次全新索引构建。计时从发出 SQL CREATE VECTOR INDEX 前开始，到该索引的第一个 ANN 查询完成；CPU 中位数为 **8.451989921 秒**，GPU 为 **3.042239629 秒**，CPU/GPU 时间比为 **2.7782130773**。每轮完整 24 查询的 Recall@10，CPU 为 **239/240**，GPU 为 **240/240**。两条路径均通过正常数据库重开验证。

这些是已实测的完整集成版本 `5ee727` 的结果。已另行抽取的五文件 AMD-only 公共补丁能够应用到官方固定基线，但尚未单独全量构建或执行；不能把本报告冒充该未构建变体的验收。官方统一负载与目标阈值尚未作为本实验输入，当前不声明比赛得分或官方达标，本文件随待审查 PR 草稿交付。

## 版本与运行条件

| 项目 | 实际身份或配置 |
| --- | --- |
| 源码 commit | `5ee7271c6f750894a3e1ff15c9d6b2afa5f44ff9` |
| 云端 stripped seekdb SHA256 | `40e2dd628db3ea68d1a9176587617586043404c33df5535e669f242123aba2d9` |
| 对应原始构建二进制 SHA256 | `7f773832b29a06294c12b99a78c2d13389635e0ab6ce066caded522aa76ad8a3` |
| GPU-only helper SHA256 | `cebf83aad448811961be9bb5e3d44fe8e53d1382194cf739f6f586dd5e2e26a9` |
| 实际验收脚本 SHA256 | `9a31526e7cc4bf0bb4969fa21b5f42a03820ff138f5f45b920e4fbbb7401f8e5` |
| AMD GPU | 实际 gfx1100，显存 51,522,830,336 字节 |
| SDK | ROCm/hipcc 7.2.4，已有匹配 hipVS C API |
| 云容器资源 | cgroup 核实 CPU 配额16核、内存59,055,800,320字节；不引用宿主总资源作为本实例配额 |
| 两模式共同参数 | `memory_budget=2G`、`cpu_count=4`、`net_thread_count=2`、host OMP/OpenBLAS/MKL threads=1 |
| 索引 | 普通 dense float32 HNSW、L2、M32、ef_construction200、`extra_info_max_size=0` |
| 查询 | 会话 `ob_hnsw_ef_search=512`，K10；回读确认实际会话值512 |
| GPU 构图 | CAGRA NN_DESCENT，graph_degree64、intermediate_degree128；CPU只转换 HNSW 上层 |
| 内存节省 | 默认开启，未使用关闭内存节省的诊断参数 |

GPU helper 是单独进程，GPU 构图显式同步，转换/导出后由 seekdb 原生 VSAG 导入图，并以公开 UpdateId 两阶段恢复数据库的 native VID。查询由原生 seekdb/VSAG 继续执行，不是在查询结果外部用 offset 替换原始 ID；CPU 默认路径不先完整构图后再注册 GPU。两个模式使用相同已封存二进制；CPU 模式关闭 helper 环境变量，GPU 模式显式指定已校验 helper 与独占 scratch 目录。

实验使用官方模板3102，其预览镜像标识为`seekdb-hipvs-nb-demo_v1.4.0@sha256:a1aa9d9378e054400c8ee7929572504dd4b1ecb4487e814ebfd87babd9f1456d`。实际安装的`libcuvs_c.so` SHA256为`b63211897276e0a6911bb534fe77c8b0b59f31c50af1df2f108845f06ad46ebf`；用于独立原生导入验证的Linux VSAG SDK包为1.1.0，运行时版本字符串为`012a0f2`，不把iOS固定提交或其他上游版本当成本次Linux运行库。

本报告不包含账户、实例域名、认证链接、密码或密钥。实例自动销毁配置未核实，不将其写为已配置。

## 输入和精确 oracle

数据为合成负载：N20,000、dim128、24个查询、K10、seed20261027，浮点数由固定 MT19937/IEEE float32 算法生成，范围为[-1,1]。逻辑主键为 `100000000003 + 17*i`，与连续行号不同。表中向量使用九位有效数字、保留浮点零符号的文本导入；原始2,563,072个 float32 已完成 SQL 文本回转的逐字节验证。两个模式的四个输入文件 SHA256 完全相同：

| 文件 | SHA256 |
| --- | --- |
| `base.f32` | `cc5ea4ca8bd02cdbf807f721df666ea797fdc16582d9ced4ba523f9e8c2e93e3` |
| `queries.f32` | `525d711d576289a68bf6648b522ac5f551e2ed633fa703998c7417f736629dc6` |
| `ids.i64` | `38e9a50ab1ed4f6339740f0c62ce81883911722ea190cf1b5e506c75202cdddc` |
| `exact-ground-truth.json` | `fc90f6a3662f23f7ea8b3164910b9e42dfc87cffd1b0c35fa9ad4b1df0bf5773` |

oracle 在所有20,000个原始向量上独立计算平方 L2 距离，使用完整24查询、每查询固定10个结果为分母；不是从近似返回结果生成答案，也不是比赛隐藏标注。六份实际查询结果已独立重算命中率并检查每个返回ID合法且同查询无重复：GPU三轮各240/240，CPU三轮各239/240。三份 GPU 结果文件还与精确 oracle 文件逐字节相同。

可移植生成器为 `tools/hipvs_build/bench/generate_inputs.py`，SHA256 `e401ff8ea376aae039f5c357b7418218f9ddcb74e03d1690b5a35d62a3be6490`。它仅用 Python 标准库，默认 PLAN；显式执行时只创建新的调用方自有输出目录，不依赖 D盘位置、不打包本机 Python 客户端、不安装依赖、不连接数据库或网络。摘要实现使用兼容Python3.10的分块SHA256循环；MT19937、f32、原始数据及oracle算法未改。

该文件已在现有Ubuntu22系统`/usr/bin/python3`（Python3.10.12）真实独立全量运行一次：CPU0、nice19、退出0、实际7.748707154秒、子进程峰值RSS45,696KiB。三份raw和oracle的大小/SHA256全部与上表精确相同，MT19937/f32核心AST也与原生成器相同。实际资源与四文件核对回执在`the retained local validation-py310-r1/receipt.json`。核对后仅删除该次新独占目录的四份重复raw/oracle（10,416,287逻辑字节），代码、生成日志和两级receipt保留，原实验输入未动。数据生成闭环完成不代表AMD-only源码树已独立完整构建或运行。

```sh
# 将已审查的生成器放在工作目录中；这里只创建当前目录下全新的实验输出。
RUN=$(mktemp -d "$PWD/amd-repro.XXXXXXXX")
python3 generate_inputs.py
python3 generate_inputs.py --execute --output "$RUN/inputs"
sha256sum "$RUN/inputs/base.f32" "$RUN/inputs/queries.f32" "$RUN/inputs/ids.i64"
```

## SQL 和计时复现规约

先按最终参赛分支的构建说明生成 seekdb，记录源码 commit、原二进制及实际执行二进制 SHA；独立 helper 用包含在补丁中的 `bash tools/hipvs_build/build.sh NEW_ABSOLUTE_BINARY` 配合已有 hipVS/ROCm 构建，不向 seekdb 进程装入 GPU 库。五文件 AMD-only 源码变体的独立完整构建与最终发布步骤仍待完成，不能用一次 apply-check 代替。

CPU/GPU分别使用新的自有数据目录、相同服务参数和独占实验 schema；通过回环随机监听确认自有 PID/启动时间、执行文件、实际端口与 SQL revision。仅在这些自有实例执行下列 SQL；本报告中的示例不会启动、重启或删除用户数据库。

在新的自有schema中先创建`docs`表：

```sql
CREATE TABLE docs(id BIGINT PRIMARY KEY, group_id INT, embedding VECTOR(128));
```

以下标准库转换将raw文件写成500行一批的可导入SQL，输出只保存在新实验目录；已有MySQL兼容客户端可导入`$RUN/load.sql`，无需将本机客户端打包进公开材料。`SQL_PORT`和`OWNED_SCHEMA`必须取本轮自有实例及新schema的实际值。

```sh
python3 - "$RUN/inputs" > "$RUN/load.sql" <<'PY'
from array import array
from pathlib import Path
import math, sys
p = Path(sys.argv[1])
vectors, ids = array('f'), array('q')
vectors.frombytes((p / 'base.f32').read_bytes())
ids.frombytes((p / 'ids.i64').read_bytes())
if sys.byteorder != 'little':
    vectors.byteswap(); ids.byteswap()
assert len(ids) == 20000 and len(vectors) == 20000 * 128
assert all(math.isfinite(x) for x in vectors)
def number(x):
    return ('-0.0' if math.copysign(1, x) < 0 else '0.0') if x == 0 else format(x, '.9g')
for start in range(0, len(ids), 500):
    rows = []
    for i in range(start, min(start + 500, len(ids))):
        v = '[' + ','.join(number(x) for x in vectors[i*128:(i+1)*128]) + ']'
        rows.append('(' + str(ids[i]) + ',' + str(ids[i] % 3) + ",'" + v + "')")
    print('INSERT INTO docs VALUES ' + ','.join(rows) + ';')
PY
# 仅对已新建并确认属于本轮的schema导入，不能指向用户既有数据库。
mysql --protocol=tcp -h 127.0.0.1 -P "$SQL_PORT" -u root "$OWNED_SCHEMA" < "$RUN/load.sql"
```

```sql
-- 全部20,000条载入完成并确认所有ID后，再开始三轮建索引计时。
SET SESSION ob_hnsw_ef_search=512;
SELECT @@ob_hnsw_ef_search;
CREATE VECTOR INDEX idx_amd_0 ON docs(embedding)
  WITH(distance=l2,type=hnsw,lib=vsag,m=32,
       ef_construction=200,ef_search=512,extra_info_max_size=0);
-- 把下式的参数绑定为queries.f32第一条128维向量的九位有效数字文本。
SELECT id FROM docs
  ORDER BY l2_distance(embedding, ?) APPROXIMATE LIMIT 10;
```

使用同一个已连接客户端的单调高精度时钟，在 CREATE 前记 t0，第一个 ANN 的10条结果取回后记 t1，记录 t1-t0；连接、配置、数据生成/导入、服务启动不计入。计时包括 SQL/扫描、原始向量导出、子进程、GPU传输/核心构图/同步、CPU上层转换、图导出、父进程原生导入及VID恢复、存储序列化和首次查询可能发生的重载。其余23查询用于质量检查，不计入首查时间。

每轮必须实际创建新图。运行全部24查询并检查每个结果的10个原ID、唯一性及独立 oracle 后，DROP 本轮自有索引；以新名字创建下一轮，完成各三次后取中位数。不得复用R3导出的旧图或把辅助程序单次计时当整轮时间。保留最后一轮索引用于过滤、事务回滚/提交、删除、插入和主键变更检查，再正常关闭数据库，使用同一base的新进程重开；重开阶段只读验证，不重建索引。

完整质量口径为`sum(|returned_ids[q] ∩ exact_ids[q]|) / 240`；任何查询少于10行、同查询重复ID或返回输入域之外的ID均直接失败，不缩小分母。首次查询可复用作query0质量结果，余下23查询不改变已记录的首查计时。聚焦DML使用生成器的原向量：第一行向量置NULL后在事务内确认不再被ANN返回、ROLLBACK后恢复；提交第一行的新向量、删除第三行、以新正主键插入原第三行向量，再将该行主键改为负值。重开验证在这些已提交变化上重新计算oracle，不能拿初始oracle忽略DML。

GPU模式必须同时证明该轮日志增量中出现 `GPU graph imported with native original IDs; no CPU Build`，且没有 CPU fallback。日志读取以DDL前文件 device/inode/offset到固定结束offset为范围，完整增量计算SHA，并保存有界真实命中片段；文件轮转或截断而无法覆盖时拒绝使用上一轮标记。

另已从云端精确回收三条父进程ASCII成功原文，逐条SHA核对，保存在`same5ee-native-gpu-selected-lines.json`。去除时间、线程、trace ID及后续辅助诊断后的选句为：

```text
GPU graph imported with native original IDs; no CPU Build(count=20000, dim_=128, max_degree_=32, ...)
```

三条原行（不含换行）的SHA256依次为`a0e8e760b9fccba2cdcc47b73ce1eb10ea8ea3ce8cad854045500dcf8efc1e23`、`4fa04501c24c4aebb36b055aab7dd64f5e38c14ad31bcbd5fa1e1452257584aa`、`f9cad744dea1b914a1aee6f8e9d6e3588d8973d6a11e48f8ae6f9175af7e83bb`。来源三个完整混合字节excerpt各16,512字节，其SHA另有记录，但没有将这三个整excerpt冒称已本地回收；公开材料只引用已校验的ASCII原文。原行的1.9269/1.9130/1.9692秒是父进程内部阶段计时，不能代替下表的完整DDL至首查时间。

## 三轮产品 SQL 原值

| 路径 | 第1轮秒 | 第2轮秒 | 第3轮秒 | 中位数秒 | 完整查询Recall@10 |
| --- | ---: | ---: | ---: | ---: | --- |
| CPU-off | 8.388145212084055 | 8.47150007635355 | 8.451989920809865 | 8.451989920809865 | 各239/240 |
| GPU native build | 3.062071142718196 | 3.035283913835883 | 3.0422396287322044 | 3.0422396287322044 | 各240/240 |

两次初始会话各97/97检查通过，GPU三轮当轮增量中 native-import=true、fallback=false，CPU三轮两者均false。CPU/GPU中位数相除为2.7782130772953186；这是本合成负载上的完整SQL构建结果，不是上游 Notebook 查询倍率，也不是R3独立组件约7.3165倍。

本次顺序为GPU三轮及其正常重开，再执行同二进制CPU三轮及其正常重开；没有并行运行这些数据库。每轮新建索引，但没有人为清操作系统缓存。负载为已披露的20,000×128合成输入，不将这一规模的结果外推为未公布官方数据规模上的成绩。

## 正常重开和失败回退

CPU与GPU分别正常 SIGUSR1 停止，安全信号状态及退出0均有记录；然后以原base、新PID和启动时间重开。两条路径各31/31只读检查通过，包含全部原SQL ID、已提交向量变化、删除、插入/主键变化以及完整24查询。GPU重开Recall仍为1.0，CPU仍为239/240。四个会话最后均正常退出0、所有自有子进程停止，未改动用户既有数据库。

同一 `5ee727` 版本在本机以 `/usr/bin/false` 作为明确失败的辅助程序、保持默认内存节省设置，实际43/43检查通过；当轮日志 native-GPU=false、explicit-CPU-fallback=true，Recall239/240，正常停止退出0。该单轮CPU回退耗时11.8661秒，不计入云端GPU加速比。这证明已验证的非零退出回退情形，不将其扩大为所有故障类型均已测试。

早期 `bc5e` 默认回退首查零行、R2导出abort及质量不匹配、R4错误距离形状自测全部保留原FAILED记录；后续修复与正确接口验证没有改写失败历史。

## 原始记录与公开交付边界

原始14文件回收包 `same5ee-native-receipt.json` 的SHA256为 `9eb687dedeb8c8f2a8f644bae3bb229e0df600c3af700bb6c51725bacddc86ef`。其四份session、四份SQL report和六份24查询结果已经与云端逐文件长度和SHA核对，派生摘要位于 `same5ee-native-summary.json`；摘要不代替原件。报告内的完整日志增量SHA与命中标记、以及另行精确回收的三条ASCII原文，应随最终公开复现材料一并提供，不能把选句描述为完整服务器日志或完整混合字节excerpt。

AMD-only 源码提取物为 `amd-only.patch`，SHA256 `ce1ef8237cdedb455e7146596212fdda9da721a9d927856721558031a1e44aae`，固定官方基线 `5bce9e06d1281347ad61344e4b01fab9158c58a2`。它只含两个产品CPP、三个helper文件及prefix窄修，正反apply-check已过、预镜像与官方字节相同；尚无这个独立树的全量构建/SQL结果。本分支仍需补充独立完整构建与聚焦验收；公开可执行复现命令见本目录 README。未经这些步骤，本报告只陈述已实测完整版本和源补丁审计状态，不声明AMD整题完成或得分。
