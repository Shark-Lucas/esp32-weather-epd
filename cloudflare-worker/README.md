# OpenWeather Cloudflare Worker 中转

设备通过 HTTPS 请求 Worker，Worker 使用自己的 OpenWeather Key 请求上游，
并用 Workers KV 保存最近成功的 JSON。支持此项目的 One Call 3.0、4.0、
小时/日预报、告警详情和空气质量接口，响应正文保持原有格式。

Worker 已提供控制台和命令行两种部署方式。ESP32 固件已接入
`weather.hlcoding.cn`，使用经过证书验证的 HTTPS 和独立设备口令。

## 需要哪些凭据

| 名称 | 用途 | 放在哪里 |
| --- | --- | --- |
| Cloudflare 登录 | 创建和部署 Worker、KV、绑定域名 | 控制台登录或 Wrangler 浏览器登录 |
| `OPENWEATHER_API_KEY` | Worker 向 OpenWeather 请求数据 | Worker Secret |
| `DEVICE_TOKEN` | 设备请求 Worker 时的随机访问口令，32～256 字符，不限十六进制 | Worker Secret 和设备配置 |

写代码、本地测试和打包均不需要真实凭据。用浏览器或 `wrangler login` 部署时，
不需要生成 Cloudflare API Token，也不需要提供 Global API Key。
OpenWeather Key 对应的账号仍须拥有实际使用的接口权限和订阅。

生成设备口令的方法（Node >=22；生成后自行保存）：

```powershell
node -e "console.log(require('node:crypto').randomBytes(24).toString('base64url'))"
```

不要把真实凭据写入 `wrangler.jsonc`、代码或 Git。

## 方法一：直接用 Cloudflare 控制台

Worker 没有运行时 npm 依赖，可以直接粘贴代码部署：

1. 在 Cloudflare 中创建一个 Worker，例如 `esp32-openweather-relay`。
2. 打开代码编辑器，使用 `src/index.js` 的完整内容替换默认代码，保存部署。
3. 创建一个 Workers KV namespace，例如 `esp32-weather-cache`，在 Worker 的
   Bindings 中添加 KV 绑定，**变量名必须为 `WEATHER_CACHE`**。
4. 在 Worker 的 Variables and Secrets 中添加两个 **Secret**：
   `OPENWEATHER_API_KEY` 和 `DEVICE_TOKEN`。填写你自己的值，保存相关修改。
5. 在 Worker 的 Domains & Routes 中添加 Custom Domain，例如
   `weather.你的域名`。Cloudflare 会配置对应 DNS 和证书。
6. 按下面的“验证”步骤测试。

默认参数已经写在代码里；控制台部署无需额外添加变量。KV 或 Secret 尚未配置
完整时，天气接口返回 503；`/health` 只表示 Worker 能运行，不能证明凭据有效。

## 方法二：Wrangler 部署

需要 Node >=22。从本目录运行以下 PowerShell 命令：

```powershell
cd D:\Stu\esp32-weather-epd\cloudflare-worker
npm.cmd ci
npx.cmd wrangler login
npm.cmd run deploy
```

第一次部署时，Wrangler 会提供创建缺失 KV namespace 的选项。创建后确认
`wrangler.jsonc` 的 `WEATHER_CACHE` 绑定已保存 namespace ID，后续部署继续复用
它。也可事先创建 namespace，再把返回的 ID 填入该绑定：

```powershell
npx.cmd wrangler kv namespace create WEATHER_CACHE
```

创建 Worker 后，分别交互输入两个 Secret：

```powershell
npx.cmd wrangler secret put OPENWEATHER_API_KEY
npx.cmd wrangler secret put DEVICE_TOKEN
```

绑定自有域名可按方法一第 5 步完成。若希望通过 Wrangler 管理，在
`wrangler.jsonc` 顶层添加以下 `routes`，将示例域名换成自己的实际子域名，
然后再次部署：

```json
"routes": [
  { "pattern": "weather.example.com", "custom_domain": true }
]
```

配置默认同时启用 `workers.dev` 测试地址。大陆网络下建议对比自定义域名的
实际成功率；普通 Workers 不代表 Cloudflare 中国大陆节点。

## 验证

先访问 `https://weather.你的域名/health`，应返回 `{"ok":true}`。
然后用设备口令请求真实天气接口（将地址改成部署后的域名）：

```powershell
$taskDeviceToken = Read-Host '设备访问口令'
curl.exe -i -H "Authorization: Bearer $taskDeviceToken" "https://weather.example.com/data/4.0/onecall/current?lat=31&lon=121&lang=zh_cn&units=standard"
```

天气响应应为 OpenWeather 原有 JSON。响应头包括：

- `X-Weather-Cache: MISS`：本次从上游成功获取数据。
- `X-Weather-Cache: HIT`：使用仍在正常缓存期内的数据。
- `X-Weather-Cache: STALE`：上游暂时不可用，使用允许期限内的旧数据。
- `X-Weather-Fetched-At`：Worker 上次成功获取正文的 UTC 时间。
- `Age`：这份缓存自成功获取后经过的秒数；不是 OpenWeather 观测时间。
- `X-Weather-Stale: true`：仅在回退到旧数据时出现。

短时间内再请求一次，正常应命中缓存。KV 最终一致，刚写入时切换到另一个
Cloudflare 节点可能暂时再次访问上游，因此不应把第一次 MISS 当成缓存失效。

错误排查：401 可能是设备口令不匹配，也可能是上游 Key 无效；看 JSON 的
`error` 字段区分 `unauthorized` 和 `upstream_unavailable`。403 通常需要检查
上游权限，429 表示上游额度或频率限制，503 表示 Worker 配置尚未完整，
502 表示上游失败且无可用旧数据。上游的原始错误正文不会回传，避免带出 Key。

## 缓存、重试与接口约束

默认参数可在 `wrangler.jsonc` 或控制台的普通变量中修改：

| 变量 | 默认值 | 含义 |
| --- | --- | --- |
| `CACHE_TTL_SECONDS` | `600` | 正常缓存 10 分钟；范围 60～86400 秒 |
| `STALE_TTL_SECONDS` | `10800` | 最近成功数据最多保留并使用 3 小时；须不小于正常缓存期且不超过 86400 秒 |
| `UPSTREAM_TIMEOUT_MS` | `3500` | 每次上游尝试的总超时，包含正文读取；范围 50～10000 毫秒 |

先访问全局 OpenWeather 入口，连接失败、超时、5xx、408、异常 JSON 时换到
区域入口再试一次。两次均失败可回退旧数据。429 不立即重试，但允许旧数据；
其他 4xx 不重试、不回退，便于发现 API Key 或订阅配置问题。

所有天气请求必须为 GET 并携带 `Authorization: Bearer <DEVICE_TOKEN>`。
仅允许以下路径，不接受任意上游 URL，不转发调用者的请求头或 API Key：

```text
/data/3.0/onecall
/data/4.0/onecall/current
/data/4.0/onecall/timeline/1h
/data/4.0/onecall/timeline/1day
/data/4.0/onecall/alert/<alert-id>
/data/2.5/air_pollution
/data/2.5/air_pollution/history
```

调用者 URL 中旧的 `appid` 会被丢弃，Worker 只使用自己的 Secret。坐标、
语言、单位、小时分页的 `start`/`cnt` 等参与缓存键，避免返回另一位置或页面。
One Call 4.0 的 `cnt` 限制为 1～20；实际返回数量由上游决定。

空气质量历史查询的窗口限制为最多 48 小时。如果 `end` 距 Worker 当前时间
不超过 15 分钟，就按“位置 + 窗口长度”保存滚动查询的最近成功数据，正文中
的原始时间戳保留。明确查询更早的历史时，`start` 和 `end` 完整参与缓存键。

只缓存大小不超过 512 KiB 且通过基础结构验证的成功 JSON。
这不是完整的气象字段验证，固件原有解析器仍负责各字段校验。
返回的缓存策略为 `private, no-store`，防止绕过设备鉴权的外层 HTTP 缓存。

缓存由设备请求触发更新，没有定时任务；设备长时间离线后，上游又不可用，
过期缓存不会继续返回。KV 最终一致，不能保证每个节点立即读到最新写入。
设备到 Cloudflare 的连接失败时，Worker 端缓存也无法替代这段连接。

## 本地开发和检查

```powershell
npm.cmd ci
npm.cmd test
npm.cmd run build
Copy-Item .dev.vars.example .dev.vars
# 在本机编辑 .dev.vars，再启动：
npm.cmd run dev
```

测试会替代外部网络和 KV，不需要真实 Key，也不会请求 OpenWeather。
`build` 仅进行打包 dry-run；不会部署。`npm run dev` 使用本地 KV，不能用来
测量 ESP32 到 Cloudflare 云端的线路质量。`.dev.vars`、本地 KV、依赖和构建
产物均已加入忽略规则。

## ESP32 固件接入

`platformio/include/config.h` 中 `OWM_USE_RELAY` 默认启用；
`platformio/src/config.cpp` 的 `OWM_RELAY_HOST` 为 `weather.hlcoding.cn`。
在本机被 Git 忽略的 `platformio/include/secrets.h` 中设置
`SECRET_OWM_RELAY_TOKEN`，其值必须与 Cloudflare 的 `DEVICE_TOKEN` 完全一致。

固件继续使用独立的 One Call 3.0/4.0 解析器。中转模式下：

- 所有 OpenWeather 请求均使用 Worker 域名和 Bearer 口令，不附带上游 API Key。
- HTTPS 使用 `relay_cert.h` 中的 GTS Root R4 根证书验证服务器，TLS 保留域名
  校验和 SNI。该信任锚已验证当前域名的证书链；更换域名或证书颁发机构时
  应重新核对根证书，不能复用 OpenWeather 旧证书或关闭验证。
- DNS 不再使用 OpenWeather 的固定备用 IP。上游入口的切换由 Worker 处理。
- Wi-Fi 连接后、校时前，使用 `config.cpp` 的 `WIFI_DNS_PRIMARY`
  (`223.5.5.5`) 和 `WIFI_DNS_SECONDARY` (`119.29.29.29`)；保留 DHCP
  下发的主 DNS 为第三回退。两种连接方式（已有网络和配网完成）都会应用。
  IP 地址仍由 DHCP 分配。2026-10-30 在 ESP32 当前网络各测 5 次，中位
  解析耗时分别为 28 ms 和 34 ms，均成功 5/5；114 的两个地址为 37/47 ms。
  该结果只代表本次网络样本，DNS 配置不固定 Cloudflare 节点地址。
- 请求使用 HTTP/1.0 和 `Accept-Encoding: identity`，让 ArduinoJson 可直接
  读取未经压缩、没有 chunk framing 的正文。
- 串口输出 `[relay] cache=HIT/MISS/STALE fetched=... age=...s`。目前缓存状态
  记录在串口中，屏幕界面保持现有布局。

现有失败画面、重试次数和深度睡眠处理保留。默认 Worker 上游预算约为
两次 3.5 秒加一次最多 1.2 秒的 KV 读取；若增大 Worker 超时，也要调整
固件的响应超时。

切回直连时，注释 `config.h` 中的 `#define OWM_USE_RELAY`，并在
`secrets.h` 中保留有效的 `SECRET_OWM_API_KEY`。直连继续使用原有域名和
HTTP/HTTPS 配置；中转模式不会把这个上游 Key 编译进固件或发送到 Worker。

构建与字体检查（从 `platformio/` 运行）：

```powershell
platformio run
powershell -ExecutionPolicy Bypass -File ..\fonts\update_fonts.ps1 -Check
```

云端已在 2026-10-30 验证：无令牌返回 401；当前、小时、日、空气质量接口
均返回 200，重复请求命中 KV 缓存。当前位置为固件配置的坐标；电脑测试
不能代表 ESP32 所在网络的长期成功率。烧录后应查看串口确认实际刷新。

## 官方参考

- [Wrangler 登录和部署命令](https://developers.cloudflare.com/workers/wrangler/commands/)
- [Worker Secrets](https://developers.cloudflare.com/workers/configuration/secrets/)
- [自动创建资源绑定](https://developers.cloudflare.com/workers/wrangler/auto-provisioning/)
- [自定义域名](https://developers.cloudflare.com/workers/configuration/routing/custom-domains/)
- [KV 一致性和缓存](https://developers.cloudflare.com/kv/concepts/how-kv-works/)
