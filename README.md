# Web3 Searcher

C++ 本地区块链交易检索服务。searcher 和 updater 通过 libhv 在 `127.0.0.1` 上收发 HTTP/JSON。searcher 根据 `INDEX_DIR/schema.json` 管理 IResearch 表，在同一个 HTTP 地址接收区块更新、原始查询和配置路由请求。updater 将实时区块提交给 searcher。示例使用两笔合成的以太坊交易和一个区块，适合离线验证。

## 构建

需要 CMake 3.28+、Ninja、LLVM Clang 23 和已初始化子模块的 SereneDB 源码。默认使用同级目录 `../serenedb`，目录分工参考源码中的 `examples/iresearch/mvp`，当前适配版本为 `d068c8a057c7c999f14cbc0180e2a063d393cce2`。

```sh
cmake -S . -B build -G Ninja -DSERENEDB_SOURCE_DIR=../serenedb
cmake --build build
ctest --test-dir build --output-on-failure
```

运行全部存储组件单元测试可以用 `ctest --test-dir build -L table --output-on-failure`。单独验证 WAL 可用 `ctest --test-dir build -R '^update_log_test$' --output-on-failure`。

路由和表管理器的单元测试直接调用 handler 和 Manager，在临时目录建表。运行这些测试可用 `ctest --test-dir build -R '^(routes|internal_routes|manager)_test$' --output-on-failure`。

运行 searcher 端到端测试可用 `ctest --test-dir build -R '^searcher_storage_e2e$' --output-on-failure`。测试在临时目录和本机动态端口启动真实 searcher，通过 HTTP 验证 16 块延迟下候选高度 0–17 的查询可见性和固化高度、候选块替换、整块提交失败时的回滚、固化块覆盖，以及进程被终止后从 WAL 和磁盘检查点恢复。

仓库根目录下的 `e2e/` 存放固定场景，`fuzz/` 存放状态机随机测试。`ctest --test-dir build -L e2e --output-on-failure` 运行 searcher 存储、公开路由的端到端测试和随机测试，用 `-L fuzz` 可单独运行随机测试。随机测试默认使用 3 个固定种子。每个种子先按 16 块延迟推进，随机生成区块数据和候选替换，并在固化窗口内强制重启。随后执行 64 步随机操作，最后再次重启。每一步都按独立模型核对固化高度、哈希、其他索引字段的命中结果及分组计数的前三名。可用下面的命令指定种子和步数，或设置 `SEARCHER_FUZZ_SEED=random` 探索新序列。失败日志包含实际种子、失败步骤和复现命令。

```sh
SEARCHER_FUZZ_SEED=42 SEARCHER_FUZZ_STEPS=200 ctest --test-dir build -R '^searcher_e2e_fuzz$' --output-on-failure
```

在 Ubuntu amd64 上，先安装 `build-essential`、`cmake`、`ninja-build`、`python3`、`git` 和 `curl`，再运行相邻 SereneDB 源码中的 `sudo ../serenedb/scripts/setup_dev_toolchain.sh` 安装 Clang 23、LLD 和 compiler-rt。构建和测试使用上面的命令。SereneDB 的默认 amd64 指令集为 Haswell 级别，要求 CPU 支持 AVX2、BMI2 等指令。

首次构建会下载固定版本的 libhv v1.3.4 源码包并校验 SHA-256。libhv 在 SereneDB 的编译环境内生成静态库，服务端和客户端共享该库。

顶层 CMake 使用 `ExternalProject_Add` 调用 SereneDB 的精简构建，通过 `SDB_EXTERNAL_DIRS` 将服务和性能测试程序加入同一个编译环境。searcher 链接 `iresearch` 及其传递依赖（含 DuckDB），公共通信代码使用其随附的 simdjson。产物在 `build/bin/`。macOS 的整数类型适配在构建目录内生成，见 `cmake/darwin.cmake`。可通过 `-DWEB3_BUILD_JOBS=4` 指定依赖构建并行度，用 `-DWEB3_CLANG=...` 和 `-DWEB3_CLANGXX=...` 指定编译器。

searcher 的 `config/schema.*` 负责解析表配置。`manager/manager.*` 按表名管理并调用 `Table`。

`table/table.*` 封装建表、整块提交、读高度和查询。每张表有一个 `Solidity` 保存固化数据，一个 `Candidate`（`table/candidate.*`）按高度管理候选块的 IResearch 内存段。`table/solidity.*` 负责单表读写、恢复和发布。每张表的 `Solidity` 持有一个 `MemorySegmentManager`。`table/memory_segment.*` 负责将单个批次封存为一个内存段，`table/memory_segment_manager.*` 管理本表的内存段、累计文档数和后台内存合并线程。达到刷盘阈值时，它按写入顺序将内存段直接合并写入磁盘并同步文件。随后由 `Solidity` 发布清单和快照、回收旧 WAL。`table/snapshot.*` 管理可见记录、删除掩码和列补丁。`table/row_codec.*` 按 schema 将 JSON 文档写入索引，并将存储列读回 JSON。`table/update_log.*` 负责预写日志（WAL）的追加、重放和清理。`table/segment_merger.*` 负责调度磁盘段合并并回收旧段文件。`table/utils.*` 提供目录锁、文件同步、段 ID 解析、磁盘段写入与清单读写。首次建表由 `Solidity::Write` 直接写入种子文档。CLI 和 HTTP 路由共用同一个 `Manager`。

## 按 schema 建表

[examples/searcher/schema.json](examples/searcher/schema.json) 定义了 `transactions` 和 `blocks` 两张表。直接运行 searcher 即可构建全部表：

```sh
mkdir -p run/catalog
cp examples/searcher/{schema,transactions,blocks}.json run/catalog/
build/bin/searcher build run/catalog
```

顶层 `tables` 数组的每项包含表名 `name`、字段清单 `fields` 和可选的 `documents`。数据文件为 JSON 对象数组，路径相对于 schema 文件解析。省略 `documents` 可以建空表。启动时固定读取 `INDEX_DIR/schema.json`。对于每张表，`INDEX_DIR/<表名>/` 不存在或为空时，searcher 按 schema 导入数据并原子发布该表。有数据的表目录直接打开，原始导入文件可以离线。若现有表索引无法打开则启动报错。所有表就绪后才开始监听 HTTP。

每张表可用正整数 `flush_docs` 配置生成磁盘 segment 的记录数阈值，默认 14,400 条。重复更新同一条记录也逐条计数。计入本次请求后达到或超过阈值时，整批一起刷盘。修改 `INDEX_DIR/schema.json` 并重启 searcher 后，已有 WAL 中的记录也计入新阈值。

每张表通过 `merge_segments` 设置后台合并的段数阈值，默认 16，最小 2。每个非空更新批次生成一个内存段。后台按写入顺序将相邻的同级内存段合并为一个高一级的段，保留查询快照中的删除标记和列补丁。内存合并不改变待刷盘的文档计数，也不产生磁盘索引文件。磁盘段数达到同一阈值时，后台将当时已刷盘的段合并为一个基线段。合并保留有效行及最新列值。生成新段期间可以继续查询、写入。后台根据 IResearch 的文档 ID 映射，将合并期间发生的删除和列更新映射到基线段，并复用之后追加的磁盘段与内存段。发布时校验磁盘、固化和候选快照版本，持写锁切换清单与查询视图。合并保留当前 WAL 和区块高度。清单持久化后移除旧文件，进行中的查询持有的内存映射在快照释放时回收。启动时先恢复清单和 WAL，达到阈值的表随后由后台任务处理。

| 字段配置 | 行为 |
| --- | --- |
| `name`、`type` | 使用 DuckDB 类型，如 `BIGINT`、`BOOLEAN`、`DECIMAL(10,2)`、`DATE`、`VARCHAR[]`、`DOUBLE[2]`、`STRUCT(network VARCHAR, tags VARCHAR[])`。缺失字段和 JSON null 均按 NULL 处理。 |
| `indexed` | 设为 true 可按值查询。数组按元素匹配，结构体使用点路径，如 `metadata.tags`。 |
| `stored` | 设为 true 可在命中行中返回该字段，与是否索引独立。 |
| `analyzer` | 对已索引的 VARCHAR 指定 `"text"` 可按词查询。默认使用精确匹配，标量查询词采用该类型的文本表示，如 `19.90`。 |

## 启动与 HTTP 接口

先按上节将 schema 和示例数据放在 `run/catalog/`，离线构建步骤可省略。在仓库根目录运行。示例端口可通过启动参数更换。启动格式为 `searcher serve INDEX_DIR URL [router.conf]`。传入路由清单后，配置的业务路由与内置接口共用同一个地址；省略清单时只加载内置接口。

```sh
mkdir -p run
updater_url=http://127.0.0.1:9312
searcher_url=http://127.0.0.1:9313
build/bin/searcher serve run/catalog "$searcher_url" examples/searcher/router.conf & searcher_pid=$!
build/bin/updater serve "$updater_url" "$searcher_url" & updater_pid=$!
```

| 服务 | 接口 | 用途 |
| --- | --- | --- |
| searcher、updater | `GET /health` | 检查当前进程，成功返回 `{"ok":true}` |
| updater | `POST /candidate`、`POST /solidity` | 转发指定表的完整候选块或固化块 |
| searcher | 清单中配置的 GET / POST 路径 | 按业务参数查询，返回配置的公开字段或固化高度 |
| searcher | `POST /candidate`、`POST /solidity`、`POST /search`、`POST /block_number` | 按匹配字段更新记录、查询数据或读取表的最新区块高度 |

POST 请求使用 `Content-Type: application/json`。业务 JSON 请求体上限为 16 MiB，返回值包含 `ok`，错误响应使用 HTTP 错误状态码并包含 `error`。

searcher 的内置和配置路由共用一个表管理器和一个线程池。配置路由在进程内直接访问表管理器。searcher 使用 `web3::ThreadPool` 处理业务请求。`Commit(fn)` 将函数加入队列，返回可取得结果或异常的 `std::future`。线程池按逻辑 CPU 数创建工作线程。Linux 优先使用 CPU 亲和性掩码，支持 `taskset` 和容器的 cpuset。searcher 在启动时打开各表的 IResearch reader，并发查询复用已打开的只读索引。updater 在单独的工作线程上接收批次并异步转发更新请求。各端口的 HTTP 线程均可独立响应 `/health`。每个监听端口最多接受 64 个尚未完成的业务请求，达到上限时返回 HTTP 503。命令行工具同步等待结果。

每次请求提交一个区块在一张表中的全部文档。两类提交都使用 `table`、`block_number`、`match_field` 和 `documents`。高度使用非负整数，空区块传 `documents: []`。

- `POST /candidate` 提交候选块。同高度的新块整块替换旧块，更高的候选块保留并按高度顺序应用。已固化高度及以下的迟到候选块返回成功，`upserted` 为 0。候选块仅驻留内存，重启后可由上游重送。
- `POST /solidity` 固化完整区块。新高度必须严格大于当前固化高度，可以跳过高度。重复或回退的高度返回 HTTP 400。写入成功后，查询一次性切换到新的固化数据和所有更高的候选块。固化失败保留原查询视图。较高候选块的匹配键如果在新视图中变得不唯一，该候选块整体暂缓展示，后续替换或固化会重新计算。
- `POST /search` 只取得一次已发布快照。按高度追加候选块时，后台线程在当前查询快照上应用新块。替换或乱序插入候选块、固化或重载时，按高度在固化快照上重新应用剩余候选块。磁盘合并线程按当前版本转换已有视图。每次构建有独立的版本号，同高度替换也会产生新版本。重建期间仍可读取旧快照，固化数据可恢复且新快照发布后，固化请求才返回成功。`POST /block_number` 返回这张表最新的固化高度，新表返回 `-1`，固化区块 `0` 后返回 `0`。

### Searcher 路由

searcher 启动时读取 `examples/searcher/router.conf`。清单每行是一个 URL 定义文件，支持空行和以 `#` 开头的注释；相对路径从清单所在目录解析。例如：

```text
routes/account_transfers.json
routes/account_top_senders.json
routes/account_received_stats.json
routes/transaction_by_hash.json
routes/chain_height.json
```

每个 JSON 文件包含一个路由对象。例如 `routes/account_transfers.json`：

```json
{
  "path": "/v1/account/transfers",
  "table": "transactions",
  "query": {
    "lucene": "(from:${address} OR to:${address})",
    "limit": 20,
    "select": {"transaction_id": "hash", "block_height": "blockNumber"}
  }
}
```

默认支持 GET 和 POST。`${address}` 从 GET 的 URL 参数 `address` 或 POST JSON 的同名字段读取，作为完整 Lucene 词项绑定。表达式、表名和返回字段由配置决定。`select` 左侧为公开别名，右侧为顶层已存储字段。searcher 只读取所选列；选择整个结构体或数组时返回该列的完整 JSON 值。缺失或无效参数返回 HTTP 400。公开路由执行错误返回 HTTP 500，响应使用公开错误信息。

可选的 `methods` 数组限制请求方法，例如 `["GET"]`。省略 `params` 时，占位符自动声明为必填非空字符串。显式配置时须声明模板中的全部参数，例如 `"params":{"height":{"type":"integer","get":"h","post":"/filter/height"}}`。支持 `string`、有符号 64 位 `integer`、`boolean`。GET 的布尔值为 `true` 或 `false`，POST 的数值和布尔值使用原生 JSON 类型。`post` 支持 JSON Pointer。一个占位符可重复使用，位置为 `字段:${参数名}`。高度路由使用 `"operation":"block_number"` 和固定的 `table`。清单全部加载成功后才开始监听，配置路径与内置路径（含 `/health`）或其他配置路径重复、配置文件无效时启动失败。

路由的 `query.limit` 指定最多返回多少条记录，默认 100，允许 1 到 65535 的整数。searcher 启动时校验该配置，GET 和 POST 均使用路由配置的上限。

配置路由支持 `COUNT`、`SUM`、`AVG`、`MIN`、`MAX`、分组和排序。示例 [account_top_senders.json](examples/searcher/routes/account_top_senders.json) 按收款地址过滤，返回交易笔数最多的 10 个发送地址。可用 `GET /v1/account/top-senders?address=0x…` 或 POST 同一 URL 并传入 `{"address":"0x…"}`。成功响应沿用 `hits` 数组，例如 `{"ok":true,"hits":[{"sender":"0xaa","transfer_count":12}]}`。

[account_received_stats.json](examples/searcher/routes/account_received_stats.json) 定义了 `GET /v1/account/received-stats?address=0x…`，也支持 POST。它对发往该地址的可见交易统计笔数、有金额的笔数，以及以 wei 为单位的总额、均值和最小、最大金额。

| `query.select` 配置 | 含义 |
| --- | --- |
| `{"op":"count","field":"*"}` | 统计所有命中记录。 |
| `{"op":"count","field":"value"}` | 统计指定已存储字段的非空值，零也计入。 |
| `{"op":"sum","field":"amount"}` | 对指定列求和。`BIGNUM` 使用精确整数运算。 |
| `{"op":"avg","field":"amount"}` | `BIGNUM` 返回向零截断的整数平均值，例如 −5 除以 2 得 −2。浮点列返回 `DOUBLE`。 |
| `{"op":"min","field":"amount"}`、`{"op":"max","field":"amount"}` | 按数值大小取最小值或最大值。 |

- 字段算子忽略 `NULL`。省略 `group_by` 时返回一行。命中为空或字段全为 `NULL` 时，字段计数为 0，其他字段算子返回 JSON `null`。分组查询没有命中时返回空数组。
- `SUM`、`AVG`、`MIN`、`MAX` 仅接受 schema 中已存储的 `BIGNUM`、`FLOAT` 或 `DOUBLE` 字段，服务启动时校验。`COUNT(field)` 接受任意已存储类型。
- 示例交易的 `value` 是以 wei 为单位的 `BIGNUM`。写入方将链上 hex 金额转换成十进制整数字符串，例如 `"1000000000000000000"`。该列的明细和四个数值聚合结果均以十进制字符串返回，总和可以超过 256 位。
- `group_by` 使用顶层已存储字段名，支持多列。带分组或聚合的查询中，每个普通输出列都须出现在分组中。`NULL` 分为一组。
- `order_by` 使用 `select` 的公开别名和 `asc` 或 `desc`，按配置顺序比较，各列的 `NULL` 排在末尾。同值排序可再指定分组键。省略排序时，返回顺序未指定。
- 无分组且仅输出 `COUNT(*)` 时，全表查询累加当前快照各段的有效文档数，带 Lucene 条件的查询使用 IResearch 的计数计划。字段聚合通过 IResearch 批量读取命中行的存储列，再由 DuckDB 计算。
- 每次请求固定一个包含候选块的查询快照。聚合的 `limit` 在分组和排序之后生效，默认 100，最大 65535。聚合和排序共用 2 个执行名额，DuckDB 缓冲内存上限为 256 MiB，查询时限为 30 秒。超过计算预算返回 HTTP 503，响应超过 16 MiB 返回 HTTP 413。

### Searcher 原始查询

向 searcher 发送 `POST /search`，请求体例如 `{"table":"transactions","query":"from:0x01 OR to:0x01","fields":["hash"],"limit":20}`。省略 `table` 时查询 `transactions`，省略 `fields` 时返回全部已存储字段。显式传入的 `fields` 必须是非空、无重复的已存储字段名数组。Lucene 查询词需要指定已索引字段，`*:*` 匹配全部可见记录。

searcher 同样默认返回最多 100 条，非法 `limit` 返回 HTTP 400。整个快照共用一个上限，累计取得指定条数的可见记录后即停止遍历。普通配置路由在启动时绑定存储列和公开别名，直接从查询批次生成最终 JSON。返回顺序取决于快照中的遍历顺序。若序列化的响应超过 16 MiB，searcher 停止构建结果并返回 HTTP 413。

普通字段保留大小写并按完整词项匹配。配置 `analyzer: "text"` 的字段在查询和写入时使用相同的分词规则，支持短语，例如 `description:"sample ethereum"`。已有分词索引需要从源文档重建，才能按词位置匹配短语。范围表达式比较索引词项的字典序，数值字段也使用其文本表示。

向 searcher 发送 `POST /block_number`，请求体为 `{"table":"transactions"}`，即可读取该表当前可见的高度。未刷盘批次的高度和文档一起写入 WAL 并发布。刷盘时，高度和磁盘段列表写入同一份清单，重启后从清单开始，按顺序重放完整的 WAL 记录。

`POST /solidity` 的 `match_field` 必须是非空字段名数组，例如 `["hash"]` 或 `["hash", "blockNumber"]`。每个字段都必须是已索引的顶层标量，字段名不能重复。每条文档都要提供全部匹配字段的非 NULL 值。searcher 按 schema 转换字段值，通过 IResearch 的倒排索引查找同时精确匹配全部字段的记录，数组顺序不影响匹配。分词字段的公开搜索保留分词语义。不同请求可以在同一张表上选择不同的匹配字段。

命中一条时整行覆盖，未命中时新增；命中多条时返回 HTTP 400，整批写入撤销。同批顺序处理，缺失字段按 NULL 处理。仅改可打补丁的存储列时，使用列补丁；索引值改变时重新索引整行。WAL 和磁盘 segment 都保留每次更新使用的匹配字段。旧格式的索引目录需从源文档重新构建。

```sh
curl -sS "$searcher_url/health"
curl -sS -H 'Content-Type: application/json' \
  -d '{"table":"transactions","block_number":17,"match_field":["hash"],"documents":[{"hash":"0x01","blockNumber":"0x11"}]}' \
  "$updater_url/solidity"
# CLI 也可按高度从同一个 JSON 文档数组文件提交候选块和固化块
build/bin/updater candidate transactions 18 '["hash", "blockNumber"]' examples/searcher/transactions.json "$updater_url"
build/bin/updater solidity transactions 18 '["hash", "blockNumber"]' examples/searcher/transactions.json "$updater_url"
curl -sS "$searcher_url/v1/account/transfers?address=0x01"
build/bin/searcher query /v1/transactions/by-hash '{"id":"0x01"}' "$searcher_url"
curl -sS "$searcher_url/v1/chain/height"
```

客户端通过 `POST /solidity` 提交一个完整固化块在该表的全部文档，每次传入 `match_field` 和 `documents` 两个数组。单条写入传只有一个元素的数组。searcher 把小批更新追加到该表的预写日志（WAL），同步到磁盘后发布可查询的 IResearch 内存索引。达到该表的刷盘阈值时，当前请求将累积的记录写成磁盘 segment，再切换 manifest。低频更新保留在 WAL 中，进程重启时会自动恢复。

首次离线构建的示例结果为 `{"ok":true,"tables":2,"indexed":3}`。更新以整块为单位发布，响应中的 `upserted` 是处理的输入条数。固化块发布后，新查询立即可见，重启会从磁盘恢复。同表写入串行提交。每次查询持有一个已提交的快照，写入期间仍可查询。查询跳过已被完全覆盖的 segment，其余 segment 使用 IResearch 文档掩码过滤旧记录。匹配更新使用 IResearch 布尔查询对全部匹配字段取交集，并过滤已失效的记录。普通查询访问含有效记录的磁盘 segment 和内存中的更新。交易数组通过表的 `documents` 配置导入。普通 VARCHAR 按字段原值匹配。

```sh
kill "$searcher_pid" "$updater_pid"
wait
```

## 测试 searcher 性能

`indexer_benchmark` 通过 HTTP 直接访问 searcher。一个专用写线程先向 `/candidate` 提交高度 `h` 的候选块。当 `h >= 16` 时，随后向 `/solidity` 提交高度 `h - 16` 的整批交易。空表从候选高度 0 开始，前 16 块只提交候选块。结束时保留最近的候选块，查询线程从首轮写入完成后即可检索本轮已发布的交易。

续跑时从固化高度 `F` 推算首个候选高度 `F + 17`，首先固化 `F + 1`。压测使用高度和 `--tx-per-block` 确定性生成数据，跨次运行请使用相同的每块交易数，以便重建此前候选块的原始交易。上一轮尚未固化任何块时，从候选高度 0 重新开始。

使用独立的测试索引目录启动 searcher：

```sh
bench_dir=$(mktemp -d)
cp examples/searcher/{schema,transactions,blocks}.json "$bench_dir/"
build/bin/searcher serve "$bench_dir" http://127.0.0.1:9411 examples/searcher/router.conf
```

也可用 `./start_searcher.sh ./benchmark-index` 启动 searcher。启动脚本和压测脚本共用 `SEARCHER_URL`，默认为 `http://127.0.0.1:9411`。可通过 `ROUTER_CONFIG` 指定路由清单。

在另一个终端运行：

```sh
build/bin/indexer_benchmark http://127.0.0.1:9411 \
  --blocks 256 --query-threads 4
```

默认的 `--query-mode mixed` 依次发起三种读取，所有查询线程共享轮转次序和 `--queries N` 总次数。也可以用 `--query-mode` 选择表中的单个模式。

| 模式 | 请求 | 检查内容 |
| --- | --- | --- |
| `hash` | `/search` | 按已发布交易的哈希精确查一笔交易。 |
| `received_stats` | `/v1/account/received-stats` | 按收款地址读取交易笔数、金额笔数以及 wei 金额的和、均值与极值。 |
| `top_senders` | `/v1/account/top-senders` | 按收款地址查询交易笔数最多的 10 个发送方，校验数量和排列顺序。 |

两种聚合查询共用最多两个在途请求，与 searcher 的聚合并发名额一致。收款地址从本轮已发布的交易中确定性抽样。如果当前只有收款地址为空的交易，就检查空匹配的返回格式。混合模式和单项聚合模式需要加载示例路由。

默认写入 100 万块，每块 512 笔，共 5.12 亿笔。默认用 1 个查询线程，最后一轮写入完成后停止发起查询，并等待已发出的查询返回。`--queries N` 指定本轮总查询次数，读写各自完成指定的工作量后退出。`--queries 0` 只测写入。多块压测时最多同时发起 63 个查询，为写线程预留 1 个 HTTP 请求名额。示例交易表使用默认刷盘阈值。从空表开始，每块写入 512 笔时，候选高度达到 44 时固化第 29 块（高度 28），累计固化交易数达到 14,848 笔，触发整批刷盘。

压测期间每秒向 stderr 打印一组 `progress` 表格。第一张表每行对应一项指标，包括候选块数、固化块数、固化交易数和已完成查询数。`total` 是本轮压测的累计数量，`rate/s` 是最近一个采样区间的每秒吞吐量。第二张表按 `hash`、`received_stats`、`top_senders` 分行显示该区间的 QPS 和平均延迟（`avg_ms`，单位为毫秒）。某类查询在区间内没有完成请求时，平均延迟显示为 `-`。

结束时每种有样本的查询打印一行 `query_summary`，列出全程请求数、QPS 和 P50/P95/P99。不足一秒的短跑也会输出最终统计。stdout 用于完整 JSON 报告。

压测客户端的连接超时为 3 秒，单次 HTTP 请求总超时为 30 秒（含连接时间）。读高度、查询和写入遇到连接中断、未收到响应或 HTTP 408、504 时，最多额外重试 3 次，间隔依次为 100、200、400 毫秒。候选块超时后用同一请求体重试，同高度的候选块整块替换。固化请求超时后先读取固化高度。目标高度已提交时，客户端按哈希核对整批交易的字段。高度尚未提交时，客户端重发同一批数据。一次逻辑请求只计数一次，延迟包含重试、等待和结果核对时间。

程序校验每块的写入条数和返回高度，并按查询类型检查返回结果。成功后向 stdout 写一条 JSON，失败时以非零状态退出。`first_block` 和 `last_block` 为本轮候选高度，`last_solidity_block` 为最终固化高度。空表只跑前 16 个候选块时，该高度为 `-1`，`solidity` 统计为 `null`。`transactions_per_second` 是整个写入阶段的交易吞吐量，包含客户端生成数据、提交候选块和固化区块的时间，按本轮实际固化的交易数计数。`overlapping_searches` 是客户端观察到与区块写入重叠的查询数，每轮写入从提交候选块开始，到该轮候选提交及到期的固化操作全部完成为止。`candidate` 和 `solidity` 分别报告各自的累计请求耗时、请求数、每秒请求数和 P50、P95、P99 延迟（毫秒）。`search` 汇总所有查询线程，按查询阶段的实际经过时间计算每秒请求数。`query_types` 按三种查询分别报告请求数、吞吐和延迟，无样本的类型为 `null`。各类型吞吐使用同一个查询阶段时长，延迟从客户端取得名额并发起请求时开始计算。分位数按延迟直方图估算，取所在桶的上界，向上误差小于 1.6%。`max_ms` 为实测最长请求耗时，`over_1s` 为耗时超过 1 秒的请求数。searcher 在每次成功合并后记录 `publish_lock_us`，单位为微秒，包含最终发布时的清单持久化和快照切换。
