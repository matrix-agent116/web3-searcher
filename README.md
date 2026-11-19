# Web3 Searcher

C++ 本地区块链交易检索服务。三个进程通过 libhv 在 `127.0.0.1` 上收发 HTTP/JSON。updater 接收实时文档并按主键更新指定表，indexer 根据 `INDEX_DIR/schema.json` 用 IResearch 写入和读取各表的磁盘索引，searcher 提供字段查询。示例使用两笔合成的以太坊交易和一个区块，适合离线验证。

## 构建

需要 CMake 3.28+、Ninja、LLVM Clang 23 和已初始化子模块的 SereneDB 源码。默认使用同级目录 `../serenedb`，目录分工参考源码中的 `examples/iresearch/mvp`，当前适配版本为 `d068c8a057c7c999f14cbc0180e2a063d393cce2`。

```sh
cmake -S . -B build -G Ninja -DSERENEDB_SOURCE_DIR=../serenedb
cmake --build build
ctest --test-dir build --output-on-failure
```

在 Ubuntu amd64 上，先安装 `build-essential`、`cmake`、`ninja-build`、`python3`、`git` 和 `curl`，再运行相邻 SereneDB 源码中的 `sudo ../serenedb/scripts/setup_dev_toolchain.sh` 安装 Clang 23、LLD 和 compiler-rt。构建和测试使用上面的命令。SereneDB 的默认 amd64 指令集为 Haswell 级别，要求 CPU 支持 AVX2、BMI2 等指令。

首次构建会下载固定版本的 libhv v1.3.4 源码包并校验 SHA-256。libhv 在 SereneDB 的编译环境内生成静态库，服务端和客户端共享该库。

顶层 CMake 使用 `ExternalProject_Add` 调用 SereneDB 的精简构建，通过 `SDB_EXTERNAL_DIRS` 将本项目的三个可执行目标加入同一个编译环境。indexer 链接 `iresearch` 及其传递依赖（含 DuckDB），公共通信代码使用其随附的 simdjson。产物在 `build/bin/`。macOS 的整数类型适配在构建目录内生成，见 `cmake/darwin.cmake`。可通过 `-DWEB3_BUILD_JOBS=4` 指定依赖构建并行度，用 `-DWEB3_CLANG=...` 和 `-DWEB3_CLANGXX=...` 指定编译器。

indexer 的单表索引读写在 `services/indexer/table.*` 中。`table_manager.*` 负责按名称查表、协调并发读写，以及逐表创建或打开索引。CLI 和 HTTP 路由共用同一个 `TableManager`。

## 按 schema 建表

[examples/schema.json](examples/schema.json) 定义了 `transactions` 和 `blocks` 两张表。直接运行 indexer 即可构建全部表：

```sh
mkdir -p run/catalog
cp examples/{schema,transactions,blocks}.json run/catalog/
build/bin/indexer build run/catalog
```

顶层 `tables` 数组的每项包含表名 `name`、字段清单 `fields` 和可选的 `documents`。数据文件为 JSON 对象数组，路径相对于 schema 文件解析。省略 `documents` 可以建空表。启动时固定读取 `INDEX_DIR/schema.json`。对于每张表，`INDEX_DIR/<表名>/` 不存在或为空时，indexer 按 schema 导入数据并原子发布该表。有数据的表目录直接打开，原始导入文件可以离线。若现有表索引无法打开则启动报错。所有表就绪后才开始监听 HTTP。

每张表可用正整数 `flush_docs` 配置生成磁盘 segment 的记录数阈值，默认 256 条，示例交易表为 4096 条。同一主键重复更新也逐条计数。计入本次请求后达到或超过阈值时，整批一起刷盘。修改 `INDEX_DIR/schema.json` 并重启 indexer 后，已有 WAL 中的记录也计入新阈值。

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
| updater | `POST /update` | 将指定表的 upsert 批次异步转发给 indexer |
| searcher | `POST /search` | 用 `table`、`field`、`value` 查找记录；省略表名时使用 `transactions` |
| indexer | `POST /update`、`POST /search` | 按 schema 主键写入记录，或按字段查询 |

POST 请求使用 `Content-Type: application/json`。业务 JSON 请求体上限为 16 MiB，返回值包含 `ok`，错误响应使用 HTTP 错误状态码并包含 `error`。

indexer 和 searcher 使用 `web3::ThreadPool` 处理业务请求。`Commit(fn)` 将函数加入队列，返回可取得结果或异常的 `std::future`。两者启动时分别按逻辑 CPU 数创建工作线程。Linux 优先使用 CPU 亲和性掩码，支持 `taskset` 和容器的 cpuset。indexer 在启动时打开各表的 IResearch reader，并发查询复用已打开的只读索引。updater 在单独的工作线程上接收批次并异步转发更新请求。三个服务的 HTTP 线程均可独立响应 `/health`。每个进程最多接受 64 个尚未完成的业务请求，达到上限时返回 HTTP 503。命令行工具同步等待结果。

indexer 的健康检查成功后即可查询和更新。表级 `primary_key` 指定一个标量字段，如交易表的 `hash` 或区块表的 `height`。更新文档必须提供非空主键。相同主键整行替换，新主键插入，同批内重复键以最后一条为准。未配置主键的表可供查询。

对于已有主键，indexer 按 schema 类型比较新旧值。仅修改可打补丁的存储列（如交易的 `value`）时，保留原倒排索引并用 IResearch 列补丁发布新值，刷盘时保存列更新记录。新增主键、索引字段发生变化或修改不支持列补丁的类型时，重新索引整行。对于仅索引而不存储的字段，无法取得旧值来确认相等时也会重新索引。缺失字段仍按 NULL 处理。

```sh
curl -sS "$indexer_url/health"
curl -sS -H 'Content-Type: application/json' \
  -d '{"table":"transactions","documents":[{"hash":"0x01","blockNumber":"0x11"}]}' \
  "$updater_url/update"
# 也可以提交包含 JSON 对象数组的文件
build/bin/updater update transactions examples/transactions.json "$updater_url"
curl -sS -H 'Content-Type: application/json' \
  -d '{"table":"blocks","field":"metadata.tags","value":"offline"}' \
  "$searcher_url/search"
build/bin/searcher query transactions blockNumber 0x10 "$searcher_url"
```

客户端可以逐条调用 `POST /update`，每次传 `documents: [一条记录]`。indexer 把小批更新追加到该表的预写日志（WAL），同步到磁盘后发布可查询的 IResearch 内存索引。达到该表的刷盘阈值时，当前请求将累积的记录写成磁盘 segment，再切换 manifest。低频更新保留在 WAL 中，进程重启时会自动恢复。

首次离线构建的示例结果为 `{"ok":true,"tables":2,"indexed":3}`。更新以批次为单位发布，响应中的 `upserted` 是处理的输入条数。返回成功后，新查询立即可见，重启会从磁盘恢复。同表写入串行提交。每次查询持有一个已提交的快照，写入期间仍可查询。查询跳过已被完全覆盖的 segment，部分覆盖的 segment 用位图过滤旧记录。对已索引且未分词的主键做精确查询时，indexer 通过主键表定位最新记录，只读取命中的 segment。其他字段查询含有效记录的磁盘 segment 和内存中的更新。旧磁盘 segment 仍占用磁盘空间。交易数组通过表的 `documents` 配置导入。普通 VARCHAR 按字段原值匹配。

```sh
kill "$searcher_pid" "$updater_pid" "$indexer_pid"
wait
```
