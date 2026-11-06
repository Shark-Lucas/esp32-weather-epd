# OpenWeather 后台快照 Worker

每 15 分钟后台抓取全部已启用的位置，完整校验后合并发布到
`weather:snapshot:v2` **一个 KV key**。设备通过原有 HTTPS API 读取各接口需要的
数据，正常读取不触发 OW 请求或 KV 写入。正常全天最多 96 次快照写入；新位置
首次生成、失败写入重试另计。增加同一位置的设备不会增加上游抓取或快照写入。

代码使用免费 Workers、Workers KV 与 SQLite Durable Object，不需要付费组件。
根据项目所有者在 2026-11-06 的最新决定，**本地 OW 额度门槛已关闭**，无需先填写
额度即可后台抓取；供应商返回 429 时仍执行冷却，认证和权限错误仍正常返回。
不增加付费 Cloudflare 组件。

## 数据流

```text
Cron */15 * * * * ──┐
新位置首次访问 ────┴→ Durable Object：登记、去重、预算、抓取队列
                           ↓ 完整位置数据成功
                     WEATHER_CACHE.put（全局快照一次）
                           ↓
设备 GET → Bearer 鉴权 → KV 读取 → 提取并返回对应 API JSON
```

Cron 只提交工作；Durable Object alarm 每次处理一个位置，完成整个批次后统一
发布。一个位置不完整则保留其旧数据，不影响其他位置。所有位置都失败时不写 KV。
重复 Cron 按 15 分钟时间段去重；请求并发登记同一位置只排一个任务；网络抓取
不阻塞其他登记；发布串行执行。协调器保存批次进度，重启后继续，不依赖进程内
计时器或设备请求的存活时间。

一位置默认抓取 5 次：当前天气、20+4 条小时预报、8 天预报、24 小时空气质量。
最多再抓取 8 个去重的告警详情。第一组独立请求最多并发 4 个；每个请求总超时
默认 5 秒，覆盖正文读取。临时网络、408、5xx、重定向或数据异常最多尝试三次，
依次使用全局/区域/全局官方入口，间隔 0.5、1.5 秒。401/403 不重试；429 不立即
重试，按 Retry-After 暂停后续尝试（至少 60 秒）。只有显式启用本地预算时，才在
每次尝试之前持久化扣减额度；默认关闭该额外写入。

字段投影匹配固件独立 4.0 解析器：保留字段名、精度、时间戳、可选降水字段和
有效告警引用，删除未消费字段及告警长描述。只向 OW 传官方数量、分页等参数，
不把 `exclude` 当作 4.0 任意字段过滤器。收到正文后的裁剪减少 KV 和下行体积，
不能消除上游产生这些字段的耗时。

## 配置与免费额度

| 绑定或变量 | 默认 | 用途 |
| --- | --- | --- |
| `WEATHER_CACHE` | 必填 KV 绑定 | 原 namespace，旧 per-API key 留存用于回滚 |
| `WEATHER_COORDINATOR` | 必填 SQLite DO 绑定 | `WeatherCoordinator` 类及 `weather-snapshot-v2` migration |
| `OPENWEATHER_API_KEY` | 必填 Secret | 仅供后台访问 OW，不下发设备 |
| `DEVICE_TOKEN` | 必填 Secret | 32～256 字符，与固件设备口令一致 |
| `CACHE_TTL_SECONDS` | `900` | 超过此年龄标记 STALE |
| `STALE_TTL_SECONDS` | `10800` | 单位置完整数据最多返回 3 小时 |
| `UPSTREAM_TIMEOUT_MS` | `5000` | 单次上游总超时，范围 50～10000 ms |
| `OWM_BUDGET_ENFORCED` | `false` | 是否启用可选本地预算；默认关闭，不影响 429 冷却 |
| `OWM_BUDGET_CONFIRMED` | `false` | 仅启用本地预算时，确认账号额度后改为 `true` |
| `OWM_DAILY_BUDGET` | `0` | 仅启用本地预算时生效；0 禁止抓取 |
| `OWM_BUDGET_UNIT` | `request` | `request` 按 HTTP 尝试；`record` 按请求记录数保守预扣 |
| `OWM_BUDGET_RESET_UTC_HOUR` | `0` | 账号每日重置对应的 UTC 整点，0～23 |

默认 `OWM_BUDGET_ENFORCED=false`：不检查本地额度、不做每次上游调用的预算
持久化写入。缺少上游 Key 仍拒绝抓取。以下预算规则只在显式开启本地额度时生效：
预算未确认或额度不足时，不登记会产生抓取的新位置。
按 `request` 计量，一位置不含重试和告警至少 480 次/天；按 `record` 计量，保守
扣减每次请求的记录数量，一位置至少 5472 条/天。空气质量也计入本地预算，宁可
提前停止，不低估用量。不得假设这两种本地算法任意一种等同于账号真实计费。
先确认适用算法并预留本服务之外的调用、重试和告警额度，再启用；同时在 OW
账号设置供应商侧用量上限。运行中变更计量单位或重置整点会关闭扣减，避免重置
已用额度。状态保存在 DO，不使用 KV 逐次写入计数器。

最多保留 10 个位置配置；相同规范化 `lat/lon/lang/units` 去重。前端接受任意合法
坐标；启用可选预算时还检查额度可支撑的位置数，超出返回明确错误。7 天无人访问暂停
抓取，保留记录和最后快照，再访问时恢复。访问日期每天最多持久化一次；跨日
首次访问会通知 DO，其余当日普通读取只访问 KV。

全局快照 UTF-8 上限 512 KiB。超限拒绝发布，旧快照保持有效。新快照在单个
key 内整体替换；KV 最终一致，各节点可能短暂看到旧版本或不存在结果，读端
可等待传播，但不会因此重复回源。DO 的一份协调状态保留完整快照及待发布批次。
免费层还有 Worker 请求、KV 读取、DO 请求/存储等额度，设备数量无限并不等于
所有资源无限；测试和状态查询也占读取/请求额度。

## API 与首次启动

所有天气及状态请求都要求 `Authorization: Bearer <DEVICE_TOKEN>`。
`/health` 公开，仅检查进程，不证明 Key 或天气数据可用。只允许 GET。

```text
/data/4.0/onecall/current?lat=...&lon=...&lang=en&units=standard
/data/4.0/onecall/timeline/1h?lat=...&lon=...&cnt=20[&start=...]
/data/4.0/onecall/timeline/1day?lat=...&lon=...&cnt=8[&start=...]
/data/4.0/onecall/alert/<alert-id>
/data/2.5/air_pollution?lat=...&lon=...
/data/2.5/air_pollution/history?lat=...&lon=...&start=...&end=...
/status?lat=...&lon=...&lang=en&units=standard
```

天气位置语言默认 `en`，单位默认 `standard`，与当前固件一致；界面语言独立。
空气质量省略语言/单位时可复用同坐标天气配置，不重复登记。小时记录按 `start`
切片，`cnt` 最大 20，快照覆盖 24 小时。滚动 24 小时空气质量查询复用最近完整
窗口且保留真实时间戳；明确历史查询只返回快照实际覆盖范围。超出范围不回源。
Worker 的 `/data/3.0/onecall` 返回 410 `onecall_v3_not_enabled`；固件原独立 3.0
直连适配器仍保留。告警接口只读已抓取的详情，不因任意 ID 触发额外请求。

首次新位置返回 `503`、`X-Weather-Error: snapshot_warming` 和 `Retry-After: 5`，
后台立即排队，2 秒窗口内的登记合并一批。抓取失败后由下一次 Cron 再试，设备
轮询不会重新抓取。KV 传播过程中也可能暂时返回准备中。

设备固件保持原 API 路径及解析器，收到准备中后显示一次中文等待页，每 5 秒
重试，最长 120 秒，不消耗原有网络失败重试次数。到期显示“天气缓存准备超时”
并沿用深度睡眠。TLS 验证、HTTP/1.0、identity 编码和普通错误处理继续保留。

天气成功响应头：`Age`、`X-Weather-Fetched-At`、`X-Weather-Cache: HIT/STALE`、
`X-Weather-Generation`、`Server-Timing: kv;dur=...`；STALE 另有
`X-Weather-Stale: true`。时间是该位置抓取成功时间，不是全局发布或观测时间。
`/status` 返回指定位置阶段、最近失败、发布失败和当日预算，不登记或触发抓取。

常见错误分类由 `X-Weather-Error` 给出，不回传上游 URL、Key 或任意错误正文：

| 分类 | 含义 |
| --- | --- |
| `snapshot_warming` | 正在准备或等待 KV 传播 |
| `snapshot_expired` | 位置数据已超过 3 小时 |
| `snapshot_range_unavailable` | 查询超出快照范围 |
| `upstream_budget_not_configured` | 尚未核实/配置 OW 额度 |
| `upstream_budget_insufficient` | 无法支撑新增位置的刷新频率 |
| `upstream_budget_exhausted` | 本周期预算已耗尽 |
| `upstream_budget_unit_changed` | 计量单位或重置配置已更改，需处理已有计数 |
| `location_capacity_reached` | 已登记 10 个配置 |
| `kv_unavailable` / `snapshot_invalid` | KV 故障或快照异常 |
| `upstream_timeout` / `upstream_fetch_failed` | 后台 OW 请求超时/网络失败，状态接口可见 |
| `upstream_http_401/403/429` | OW 认证、权限或限流错误 |

`-1 Connection Refused`、TLS EOF、TCP 连接超时属于设备到 CF 的连接错误，
后台快照可减少等待上游，但不能修复这段网络。错误页保留原因、原始代码、
本次耗时及接口；底层原因不明时不猜测。无需插串口读取这些内容。

## 部署与回滚

先完成所有本地检查。仓库 `wrangler.jsonc` 已核对并绑定现有生产 Worker `weather`、
`weather.hlcoding.cn` 和 KV `esp32-weather-cache`；本地 OW 预算默认关闭。不要
另外创建新服务替换现有域名。

1. `npx.cmd wrangler login`，核实当前账号。
2. 在控制台确认绑定 `weather.hlcoding.cn` 的实际 Worker 名称、现有 KV namespace
   ID、Secret 名称和免费计划；将实际名称、KV ID、既有域名 routes 写入配置。
   Secret 保留服务上的现有值，不提交到仓库。
3. 默认保留 `OWM_BUDGET_ENFORCED=false`，按所有者要求不设本地额度门槛。若以后
   要启用预算，核实 4.0 实际额度、单位与重置时间后再配置，不以 3.0 文档推断。
4. 部署代码、SQLite DO migration、绑定及 `*/15 * * * *` Cron；不启用付费升级。
5. 使用设备原口令请求当前位置 current，让后台首次填充。轮询 `/status`，确认
   准备完成后 current/小时两页/8 天/空气质量及有效告警都能从同一快照取得。
6. 查看结构化日志 `weather_snapshot_published`（batch、generation、locations、
   bytes、kvWrites）与 Cloudflare KV/DO 指标。等待一个正常 Cron，确认每批仅
   一次快照写入、设备重复读取不增加 OW 调用或 KV 写入，且无免费执行限额错误。
7. 云端验证通过后再上传固件，串口取本机当前检测端口，不写死历史 COM 号。

旧 `owm:v1:*` key 不删除。回滚时还原旧 Worker 版本并移除 Cron 触发，避免旧
读写方案与新后台同时消耗额度；保留 DO 数据和 v2 key，便于排查及重新升级。
不得在未确认云端资源绑定时覆盖正在服务设备的 Worker。

## 本地验证

```powershell
npm.cmd ci
npm.cmd test
npm.cmd run test:runtime
npm.cmd run test:firmware
```

单元测试覆盖并发去重、发布次数、完整失败回退、跨日/跨小时、记录投影、告警、
预算、过期、KV 故障/延迟、DO 恢复。运行时测试使用 Wrangler 自带的 workerd /
Miniflare，验证真实 SQLite DO alarm 和 KV，外部网络用合成响应替代，无真实 Key。
`test:firmware` 使用已安装的 g++ 和 PlatformIO ArduinoJson，直接编译实际固件
解析器，仅替代网络与 Arduino IO，对比精简前后所有消费字段的解析结果。
`test:runtime` 先打包 dry-run，然后本地运行，不部署、不创建云端任务。

固件与字体：

```powershell
# 仓库根目录
g++.exe -std=c++11 cloudflare-worker/test/relay_warmup.cpp -o "$env:TEMP\weather-warmup.exe"
& "$env:TEMP\weather-warmup.exe"
python fonts/check_error_screen.py
powershell -ExecutionPolicy Bypass -File fonts/update_fonts.ps1 -Check
cd platformio
platformio run
```

新增中文时使用 `fonts/update_fonts.ps1 -Apply`，必要时 clean 后重编译以确保新
字库进入固件。`.pio`、本地运行时数据、依赖、Secret 和 `.dev.vars` 不提交。

官方依据：[Cron](https://developers.cloudflare.com/workers/configuration/cron-triggers/)、
[KV 限制](https://developers.cloudflare.com/kv/platform/limits/)、
[KV 最终一致性](https://developers.cloudflare.com/kv/concepts/how-kv-works/)、
[DO alarms](https://developers.cloudflare.com/durable-objects/api/alarms/)、
[DO 免费额度](https://developers.cloudflare.com/durable-objects/platform/pricing/)、
[One Call 4.0](https://openweathermap.org/api/one-call-4)。

## 2026-11-06 上线验证

已部署到原 `weather`，复用现有域名、KV 和 Secret；本地 OW 额度检查按所有者
最新要求关闭，429 冷却保留。16:30（Asia/Shanghai）实际 Cron 自动更新成功，
16:30:28 生成第 2 代快照；当前、小时 20+4 条、8 天及空气质量 24 条接口均为
`200/HIT`、同一代数据。旧 per-API key 未删除。

此次真实接口样本投影前后总量为 15,886 → 12,691 字节（约减少 20%），以小时
预报减少最明显；不是对所有天气响应的固定比例保证。COM6 配套固件烧录校验
通过，单轮 5 个请求均命中快照、完成解析和正常休眠。31 项单元测试及 10 个
位置的本地 SQLite DO/alarm/KV 验证通过；后者是合成数据测试，不代表线上已有
10 个位置或不限设备数量的负载验证。
