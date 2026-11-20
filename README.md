# Web3 Searcher

C++ 本地区块链交易检索服务。三个进程通过 libhv 在 `127.0.0.1` 上收发 HTTP/JSON。updater 接收实时文档并按请求指定的字段更新记录，indexer 根据 `INDEX_DIR/schema.json` 用 IResearch 写入和读取各表的磁盘索引，searcher 提供字段查询。示例使用两笔合成的以太坊交易和一个区块，适合离线验证。

## 构建

需要 CMake 3.28+、Ninja、LLVM Clang 23 和已初始化子模块的 SereneDB 源码。默认使用同级目录 `../serenedb`，目录分工参考源码中的 `examples/iresearch/mvp`，当前适配版本为 `d068c8a057c7c999f14cbc0180e2a063d393cce2`。

```sh
cmake -S . -B build -G Ninja -DSERENEDB_SOURCE_DIR=../serenedb
cmake --build build
ctest --test-dir build --output-on-failure
```

运行全部存储组件单元测试可以用 `ctest --test-dir build -L storage --output-on-failure`。单独验证 WAL 可用 `ctest --test-dir build -R '^update_log_test$' --output-on-failure`。

在 Ubuntu amd64 上，先安装 `build-essential`、`cmake`、`ninja-build`、`python3`、`git` 和 `curl`，再运行相邻 SereneDB 源码中的 `sudo ../serenedb/scripts/setup_dev_toolchain.sh` 安装 Clang 23、LLD 和 compiler-rt。构建和测试使用上面的命令。SereneDB 的默认 amd64 指令集为 Haswell 级别，要求 CPU 支持 AVX2、BMI2 等指令。

首次构建会下载固定版本的 libhv v1.3.4 源码包并校验 SHA-256。libhv 在 SereneDB 的编译环境内生成静态库，服务端和客户端共享该库。

顶层 CMake 使用 `ExternalProject_Add` 调用 SereneDB 的精简构建，通过 `SDB_EXTERNAL_DIRS` 将本项目的三个可执行目标加入同一个编译环境。indexer 链接 `iresearch` 及其传递依赖（含 DuckDB），公共通信代码使用其随附的 simdjson。产物在 `build/bin/`。macOS 的整数类型适配在构建目录内生成，见 `cmake/darwin.cmake`。可通过 `-DWEB3_BUILD_JOBS=4` 指定依赖构建并行度，用 `-DWEB3_CLANG=...` 和 `-DWEB3_CLANGXX=...` 指定编译器。

indexer 的 `config/schema.*` 负责解析表配置。`manager/manager.*` 负责逐表创建、打开和按名称查表。`storage/storage.*` 负责单表读写、恢复和发布。每张表的 `Storage` 持有一个 `MemorySegmentManager`。`storage/memory_segment.*` 负责将单个批次封存为一个内存段，`storage/memory_segment_manager.*` 管理本表的内存段、累计文档数和后台内存合并线程。达到刷盘阈值时，它按写入顺序将内存段直接合并写入磁盘并同步文件。随后由 `Storage` 发布清单和快照、回收旧 WAL。`storage/snapshot.*` 管理可见记录、删除掩码和列补丁。`storage/row_codec.*` 按 schema 将 JSON 文档写入索引，并将存储列读回 JSON。`storage/update_log.*` 负责预写日志（WAL）的追加、重放和清理。`storage/segment_merger.*` 负责调度磁盘段合并并回收旧段文件。`storage/utils.*` 提供目录锁、文件同步、段 ID 解析、磁盘段写入与清单读写。首次建表由 `Storage::Write` 直接写入种子文档。CLI 和 HTTP 路由共用同一个 `Manager`。

## 按 schema 建表

[examples/schema.json](examples/schema.json) 定义了 `transactions` 和 `blocks` 两张表。直接运行 indexer 即可构建全部表：

```sh
mkdir -p run/catalog
cp examples/{schema,transactions,blocks}.json run/catalog/
build/bin/indexer build run/catalog
```

顶层 `tables` 数组的每项包含表名 `name`、字段清单 `fields` 和可选的 `documents`。数据文件为 JSON 对象数组，路径相对于 schema 文件解析。省略 `documents` 可以建空表。启动时固定读取 `INDEX_DIR/schema.json`。对于每张表，`INDEX_DIR/<表名>/` 不存在或为空时，indexer 按 schema 导入数据并原子发布该表。有数据的表目录直接打开，原始导入文件可以离线。若现有表索引无法打开则启动报错。所有表就绪后才开始监听 HTTP。

每张表可用正整数 `flush_docs` 配置生成磁盘 segment 的记录数阈值，默认 256 条，示例交易表为 4096 条。重复更新同一条记录也逐条计数。计入本次请求后达到或超过阈值时，整批一起刷盘。修改 `INDEX_DIR/schema.json` 并重启 indexer 后，已有 WAL 中的记录也计入新阈值。

每张表通过 `merge_segments` 设置后台合并的段数阈值，默认 16，最小 2。每个非空更新批次生成一个内存段。后台按写入顺序将相邻的同级内存段合并为一个高一级的段，保留查询快照中的删除标记和列补丁。内存合并不改变待刷盘的文档计数，也不产生磁盘索引文件。磁盘段数达到同一阈值时，后台将当时已刷盘的段合并为一个基线段。合并保留有效行及最新列值。生成新段期间可以继续查询、写入。发布时短暂协调同表写入，把新段、合并期间追加的磁盘段和当前 WAL 接在一起。清单持久化后移除旧文件，进行中的查询持有的内存映射在快照释放时回收。启动时先恢复清单和 WAL，达到阈值的表随后由后台任务处理。

| 字段配置 | 行为 |
| --- | --- |
| `name`、`type` | 使用 DuckDB 类型，如 `BIGINT`、`BOOLEAN`、`DECIMAL(10,2)`、`DATE`、`VARCHAR[]`、`DOUBLE[2]`、`STRUCT(network VARCHAR, tags VARCHAR[])`。缺失字段和 JSON null 均按 NULL 处理。 |
| `indexed` | 设为 true 可按值查询。数组按元素匹配，结构体使用点路径，如 `metadata.tags`。 |
| `stored` | 设为 true 可在命中行中返回该字段，与是否索引独立。 |
| `analyzer` | 对已索引的 VARCHAR 指定 `"text"` 可按词查询。默认使用精确匹配，标量查询词采用该类型的文本表示，如 `19.90`。 |

## 启动与 HTTP 接口

先按上节将 schema 和示例数据放在 `run/catalog/`，离线构建步骤可省略。在仓库根目录运行。示例端口可通过启动参数更换，updater 和 searcher 的最后一个参数是 indexer 的地址。

```sh
mkdir -p run
indexer_url=http://127.0.0.1:9311
updater_url=http://127.0.0.1:9312
searcher_url=http://127.0.0.1:9313
build/bin/indexer serve run/catalog "$indexer_url" & indexer_pid=$!
build/bin/updater serve "$updater_url" "$indexer_url" & updater_pid=$!
build/bin/searcher serve "$searcher_url" "$indexer_url" & searcher_pid=$!
```

| 服务 | 接口 | 用途 |
| --- | --- | --- |
| 三个服务 | `GET /health` | 检查当前进程，成功返回 `{"ok":true}` |
| updater | `POST /upsert`、`POST /update` | 将指定表的 upsert 批次异步转发给 indexer |
| searcher | `POST /search`、`POST /block_number` | 按字段查找记录，或按表读取最新区块高度 |
| indexer | `POST /upsert`、`POST /update`、`POST /search`、`POST /block_number` | 按匹配字段更新记录、查询数据或读取表的最新区块高度 |

POST 请求使用 `Content-Type: application/json`。业务 JSON 请求体上限为 16 MiB，返回值包含 `ok`，错误响应使用 HTTP 错误状态码并包含 `error`。

indexer 和 searcher 使用 `web3::ThreadPool` 处理业务请求。`Commit(fn)` 将函数加入队列，返回可取得结果或异常的 `std::future`。两者启动时分别按逻辑 CPU 数创建工作线程。Linux 优先使用 CPU 亲和性掩码，支持 `taskset` 和容器的 cpuset。indexer 在启动时打开各表的 IResearch reader，并发查询复用已打开的只读索引。updater 在单独的工作线程上接收批次并异步转发更新请求。三个服务的 HTTP 线程均可独立响应 `/health`。每个进程最多接受 64 个尚未完成的业务请求，达到上限时返回 HTTP 503。命令行工具同步等待结果。

每张表独立记录最新成功提交的 `block_number`，新表返回 `-1`，表示尚未提交任何区块。首次成功提交区块 `0` 后返回 `0`。每次 Upsert 都在请求顶层传入整批的区块高度，使用非负 JSON 整数，可在同一高度分批写入，也可跳过高度。低于当前高度的写入返回 HTTP 400。空文档批次可以推进高度。成功响应包含 `upserted` 和 `block_number`。

向 indexer 或 searcher 发送 `POST /block_number`，请求体为 `{"table":"transactions"}`，即可读取该表当前可见的高度。未刷盘批次的高度和文档一起写入 WAL 并发布。刷盘时，高度和磁盘段列表写入同一份清单，重启后从清单开始，按顺序重放完整的 WAL 记录。

`POST /upsert` 的 `match_field` 必须是非空字段名数组，例如 `["hash"]` 或 `["hash", "blockNumber"]`。每个字段都必须是已索引的顶层标量，字段名不能重复。每条文档都要提供全部匹配字段的非 NULL 值。indexer 按 schema 转换字段值，通过 IResearch 的倒排索引查找同时精确匹配全部字段的记录，数组顺序不影响匹配。分词字段的公开搜索保留分词语义。不同请求可以在同一张表上选择不同的匹配字段。

命中一条时整行覆盖，未命中时新增；命中多条时返回 HTTP 400，整批写入撤销。同批顺序处理，缺失字段按 NULL 处理。仅改可打补丁的存储列时，使用列补丁；索引值改变时重新索引整行。WAL 和磁盘 segment 都保留每次更新使用的匹配字段。旧格式的索引目录需从源文档重新构建。

```sh
curl -sS "$indexer_url/health"
curl -sS -H 'Content-Type: application/json' \
  -d '{"table":"transactions","block_number":17,"match_field":["hash"],"documents":[{"hash":"0x01","blockNumber":"0x11"}]}' \
  "$updater_url/upsert"
# 也可以提交包含 JSON 对象数组的文件
build/bin/updater update transactions 17 '["hash", "blockNumber"]' examples/transactions.json "$updater_url"
curl -sS -H 'Content-Type: application/json' \
  -d '{"table":"blocks","field":"metadata.tags","value":"offline"}' \
  "$searcher_url/search"
build/bin/searcher query transactions blockNumber 0x10 "$searcher_url"
```

客户端通过 `POST /upsert` 批量提交同一张表的文档（`POST /update` 也接受同样的请求），每次传入 `match_field` 和 `documents` 两个数组。单条写入传只有一个元素的数组。indexer 把小批更新追加到该表的预写日志（WAL），同步到磁盘后发布可查询的 IResearch 内存索引。达到该表的刷盘阈值时，当前请求将累积的记录写成磁盘 segment，再切换 manifest。低频更新保留在 WAL 中，进程重启时会自动恢复。

首次离线构建的示例结果为 `{"ok":true,"tables":2,"indexed":3}`。更新以批次为单位发布，响应中的 `upserted` 是处理的输入条数。返回成功后，新查询立即可见，重启会从磁盘恢复。同表写入串行提交。每次查询持有一个已提交的快照，写入期间仍可查询。查询跳过已被完全覆盖的 segment，其余 segment 使用 IResearch 文档掩码过滤旧记录。匹配更新使用 IResearch 布尔查询对全部匹配字段取交集，并过滤已失效的记录。普通查询访问含有效记录的磁盘 segment 和内存中的更新。交易数组通过表的 `documents` 配置导入。普通 VARCHAR 按字段原值匹配。

```sh
kill "$searcher_pid" "$updater_pid" "$indexer_pid"
wait
```
