# AYVideo 工业级设计（V1 FFmpeg 最小播放）

> **状态（2026-08-14）**：V1 FFmpeg 最小播放 — FFmpeg demux/decode 后端 + DecodeLoop（std::thread）+ SPSC FrameQueue + `pullFrame` 时钟门控呈现 + 事件回调 + 合成 mpeg4 stub（A-04）+ **544/544 PASS × 3-run stable** + G-01 guard 双向验证绿。V0.5 stub 行为保留（无 ffmpeg 时 Null/Mock only）。
>
> **不负责（本次）**：audio-master A/V sync、AYAudio PCM 桥、渲染上屏、seek 帧精确、网络流、字幕、AAC 合成轨稳定性 —— 按 §3 进入 V2+。

---

## 目录

1. [目标与非目标](#1-目标与非目标)
2. [设计原则](#2-设计原则)
3. [Phase 路线图](#3-phase-路线图)
4. [模块边界与依赖图](#4-模块边界与依赖图)
5. [命名空间与命名约定](#5-命名空间与命名约定)
6. [核心数据结构](#6-核心数据结构)
7. [Demux 设计](#7-demux-设计)
8. [Decode 设计](#8-decode-设计)
9. [A/V Sync](#9-av-sync)
10. [播放器状态机](#10-播放器状态机)
11. [音频集成（V2 normative）](#11-音频集成v2-normative)
12. [渲染集成预留（V3 normative）](#12-渲染集成预留v3-normative)
13. [资源集成（Q3a 决策）](#13-资源集成q3a-决策)
14. [线程契约](#14-线程契约)
14.5 [目录结构](#145-目录结构)
15. [ECS 集成（V2+ foresight）](#15-ecs-集成v2-foresight)
16. [性能预算](#16-性能预算)
17. [Phase ship checklist](#17-phase-ship-checklist)
18. [反模式表](#18-反模式表)
18.5 [新增模块要求](#185-新增模块要求)
19. [验证方式](#19-验证方式)
20. [Normative amendments](#20-normative-amendments)
21. [Changelog](#21-changelog)

---

## 1. 目标与非目标

### 1.1 目标

1. **流式实时视频播放**：解封装 → 解码 → 帧队列 → 渲染/音频消费的全链路，以**帧为单位**驱动（非逐像素、非离线）。
2. **工业级质量**：结果码纪律、线程契约、确定性测试、性能预算、公开头零第三方泄漏 —— 对齐 AYVoxel / AYAnimation 既有标准。
3. **后端可插拔**：Demuxer / Decoder 双 seam（`IAYVideoDemuxer` / `IAYVideoDecoder`），Null / Mock / FFmpeg 三套后端，工厂注入 —— 测试不依赖真实媒体文件，CI 确定性。
4. **V0.5 零第三方依赖**：骨架期不链 ffmpeg，全模块 0 错 0 警纯自研编译（Q1a 决策：ffmpeg 后端 V1 进入，vcpkg 管理）。
5. **与既有生态独立并存**：与 AYResource 的 `.ayvideo`（离线 cook 帧包）**互不干扰**，新模块独立工作（Q3a 决策）。

### 1.2 非目标（V0.5 阶段明确不做）

| # | 非目标 | 进入 Phase |
|---|---|---|
| N-01 | 真实 FFmpeg demux/decode | V1 |
| N-02 | audio-master A/V sync + AYAudio PCM 桥 | V2 |
| N-03 | 渲染上屏（cross-module 纹理桥） | V3 |
| N-04 | seek 帧精确定位 / 双向 seek 优化 | V1 基础 / V4 精修 |
| N-05 | 网络流（http/rtsp/…） | V5 |
| N-06 | ECS VideoSubsystem 组件接入 | V2+ |
| N-07 | 转码 / 剪辑 / 逐帧导出 / 离线 `.ayvideo` cook | 永久不做（AYResource 分工） |
| N-08 | 字幕轨（软字幕渲染） | V4 foresight 预留 |
| N-09 | 硬解（DXVA/CUDA） | 长期 foresight，FFmpeg 软解优先 |
| N-10 | 多音轨 / 多视频轨自由切换 | V4 foresight |

---

## 2. 设计原则

### 2.1 公开头零第三方泄漏（normative，G-01）

- `include/` + `interface/` 下**任何头不得出现** ffmpeg 类型、`extern "C"` 块、`#include <libav*>` / `"libav*"`。
- 硬门禁：CMake 自定义 target `ayvideo_check_no_ffmpeg_in_public_headers`（`cmake/CheckNoFFmpegInPublicHeaders.cmake`，14 个 forbidden pattern 扫描 include/ + interface/），任何 PR 引入泄漏即 build FAIL（仿 AYVoxel CheckNoBgfxInPublicHeaders 先例）。
- ffmpeg 只允许出现在 `src/backend/`（V1 起），通过 `.cpp` 内部包含 + PIMPL 或接口隔离消化。
- **双后端盲区**：后端实现（尤其 shader/编解码）在一种后端通过 ≠ 另一种后端可编译。V1 ffmpeg 后端落地时必须双后端（Null/Mock vs FFmpeg）同时编译验证，不允许"只改了 FFmpeg 后端就没测 Null"。

### 2.2 结果码纪律（normative，G-02）

- 一切失败路径**必须有结果码**，禁止 `void` 失败路径（例外仅限 noexcept 查询如 `state()` / `rate()`，此时以 `lastResult()` 承载诊断）。
- `VideoResult` 枚举：`Ok = 0`，错误码**只追加不重排**，`Count` 永远垫底作 sentinel（§5.5 映射表）。
- 拒绝 vs 静默 clamp：**拒绝**（InvalidArgument）优于偷偷钳制 —— rate 出界拒绝并保持原值（§9.3）。

### 2.3 时间单位（normative，G-03）

- 一切媒体时间用 `ayt::time::Duration` / `TimePoint`（value type，AYTime v1.2）；禁止裸 `int64_t` 微秒在公共接口传播（内部实现可）。
- 播放位置 = `Duration`；壁钟 = `TimePoint`；`TimePoint - TimePoint = Duration`。

### 2.4 确定性硬规则（normative，G-04）

- 测试不得依赖真实壁钟：SyncClock 测试注入 `NowFn`（FakeNow 静态微秒计数）。
- 媒体内容测试用**合成字节**（Mock 后端 / V1 手写最小 MP4 容器字节），CI 无外部文件依赖。
- Mock 后端 payload 确定性：`payload[0] == 包索引`、`pts == 索引 × 40ms`（25 fps 合成流）。

### 2.5 接口与实现彻底分离（normative，G-05）

- 公共面（include/）只暴露 POD / 接口 / 门面类；实现细节（backend/、src/ 内部）不得泄漏。
- `AYVideoPlayer` 通过构造函数注入 `unique_ptr<IAYVideoDemuxer> + unique_ptr<IAYVideoDecoder>` —— 所有权明确、可测、可替换。

### 2.6 最小必要公共面（normative，G-06）

- V0.5 只 ship 当前 Phase 需要的 API；foresight 只体现在 design.md，不进头文件（§3 normative vs foresight 纪律）。
- 公共 API 一旦 ship 即受 §20 amendment 纪律保护，破坏性改动必须走 audit trail。

---

## 3. Phase 路线图

| Phase | 周期 | 交付 | 依赖 |
|---|---|---|---|
| **V0** (本次) | — | design.md 全量设计 + CLAUDE.md | 无 |
| **V0.5** (本次) | 2-3 天 | `AYVideo` 子模块仓库 + 公共头/接口 + Null/Mock 双后端 + 播放器状态机骨架 + SyncClock stub + 7 TEST_SUITE 194/194 PASS + 零第三方依赖 + guard 硬门禁 | V0 |
| **V1 FFmpeg 最小播放** | 1-2 周 | vcpkg ffmpeg demuxer/decoder 后端 + AYTask 专用解码线程 + SPSC 帧队列 + 最小播放循环（EngineClock 时钟）+ 合成字节 stub 验证（手写最小 MP4：h264 关键帧 + aac 帧） | V0.5 |
| **V2 A/V Sync + 音频** | 1 周 | audio-master 同步（§9.2/§11）+ AYAudio `openStream/streamPush/playStream` PCM 桥 + F32 SPSC 音频队列 + 控制面完整（事件回调 §10.5）+ ECS VideoSubsystem 挂载（§15） | V1 + AYAudio |
| **V3 渲染集成** | 1 周 | cross-module 纹理桥（AYRenderer）+ `IVideoFrameTexture`（§12）+ 帧纹理更新通道 + 引擎 demo 上屏验证 | V2 + AYRenderer |
| **V4 健壮性** | 1 周 | seek 帧精确 + 错误恢复/丢帧策略 + 字幕轨（foresight 升格）+ 多轨选择 + 内存压力测试 | V3 |
| **V5 网络流** | 2 周 | http/rtsp 源 + 缓冲水位 + 断流重连 —— **foresight until V5** | V4 |
| **V6+** | 后续 | 硬解 / 播放列表 / 逐帧步进 / 多实例 —— **foresight until V6** | V5 |

> **Normative vs foresight（2026-08-13）**：V0.5 以 §2 / §4–§10 / §13–§14 / §16–§19 为 **normative**。§11（V2）/ §12（V3）/ §15（V2+）为 **foresight sketch** —— 可指导接口预留，但**不得**作为 V0.5 头文件 / static_assert / ship checklist 硬门禁；进入对应 Phase 时再升格并逐条核对。
>
> **Note**：Phase 切片原则参照引擎开发规则 —— 每个 PR ship 完立即主动编译 + 跑测试。

---

## 4. 模块边界与依赖图

### 4.1 上游依赖（AYVideo → 其他模块）

| 模块 | 用途 | V0.5 实际使用 |
|---|---|---|
| **AYTime** | Duration / TimePoint / Clock（§2.3） | ✓（public link） |
| **AYTask** | V1 专用解码线程 / V1+ 异步 open-seek | V1 |
| **AYIO** | V1 文件读取（ffmpeg avio 自定义回调可选项） | V1 |
| **AYAudio** | V2 PCM 桥（`openStream/streamPush/playStream`） | V2 |
| **AYRenderer** | V3 帧纹理更新桥 | V3 |
| **AYTest** | 测试框架 | ✓（test-only link） |
| **vcpkg ffmpeg** | V1 真实 demux/decode 后端 | V1（仅 src/backend/） |

### 4.2 下游被依赖（其他模块 → AYVideo）

| 模块 | 用途 | 时机 |
|---|---|---|
| AYEntity | ECS `VideoComponent` 消费（§15 VideoSubsystem） | V2+ |
| AYEditor | 视频预览 / VideoClip 资产面板 | V3+ |
| AYApplication | 引擎 demo 上屏 | V3+ |

### 4.3 不依赖

- **不依赖 AYUI / AYAnimation / AYPhysics / AYResource**（.ayvideo 独立并存，§13）。
- V0.5 不依赖任何第三方（ffmpeg 在 V1 前不进入依赖面）。

### 4.4 Thread contract（normative table）

| 对象 | 归属线程 | 多线程访问 |
|---|---|---|
| `AYVideoPlayer` | 主线程（player thread） | 禁止；公开面单线程调用（V1 内部可起 worker） |
| `IAYVideoDemuxer` 实例 | 单实例单线程（demux thread） | 同实例禁止并发驱动；多实例可并行 |
| `IAYVideoDecoder` 实例 | 单实例单线程（decode thread） | 同实例禁止并发；ffmpeg 解码器非线程安全 |
| SPSC 帧队列 | 生产者=decode thread，消费者=player/render | **单生产者单消费者**；满则阻塞或丢帧按策略（§8.3） |
| `AYVideoSyncClock` | player thread | V1 起 position() 经原子暴露给渲染线程 |
| 事件回调（V1） | player thread 内同步触发 | 回调内禁止调用 player 控制面（重入） |

### 4.5 数据所有权表（normative）

| 数据 | 拥有者 | 生命周期 |
|---|---|---|
| 包缓冲区（`VideoPacket.data`） | demuxer 自身 | 有效至同一 demuxer 下一次 `readNextPacket` / `seek` |
| 解码帧（`VideoFrame.data`） | decoder 自身 | 有效至下一次 `dequeueFrame` / `flush`（V1 引入帧槽轮转后改为槽租约） |
| 帧队列元素 | 队列 | V1：引用计数或拷贝（解码后格式换算） |
| 媒体元数据 `MediaInfo` | player（拷贝给查询者） | `getMediaInfo` 输出副本 |

---

## 5. 命名空间与命名约定

### 5.1 命名空间

- 全部公共 API 在 `namespace ayt::video`。
- 后端实现类（Null/Mock）在 `ayt::video` 内，但头文件仅限 `backend/`（测试 include 用相对路径 `"../backend/Xxx.h"`）。

### 5.2 文件命名

- 公共头 `include/AYVideo*.h`（AY 前缀平铺，如 `AYVideoTypes.h`）；接口 `interface/IAYVideo*.h`；实现 `src/AYVideo*.cpp`；后端 `backend/XxxDemuxer.h/.cpp`；测试 `unittest/Test_*.cpp`。

### 5.3 类命名

- 大写开头：`AYVideoPlayer` / `AYVideoSyncClock` / `MediaInfo` / `VideoFrame` / `VideoPacket` / `NullDemuxer` / `MockDecoder`。
- 私有成员 `_` 小驼峰：`_state` / `_demuxer` / `_emitted` / `_anchorWall`。

### 5.4 结果码（normative，B-01 映射表）

| 码 | 值 | 语义 | 触发方 |
|---|---|---|---|
| `Ok` | 0 | 成功 | 全部 |
| `InvalidArgument` | 1 | 参数非法（rate 出界、非法路径） | Player.setRate / V1 各 open |
| `UnsupportedFormat` | 2 | 容器/编码不支持 | V1 Demuxer / Decoder |
| `DemuxError` | 3 | 解封装失败（open 失败、包读取损坏） | Demuxer / Player.open 透传 |
| `DecodeError` | 4 | 解码失败 | V1 Decoder |
| `EndOfStream` | 5 | 流耗尽（非错误） | Demuxer.readNextPacket / Decoder flush 后 |
| `InvalidState` | 6 | 状态机非法转换 / 未 open 即操作 / AudioMaster 未到 V2 | Player / SyncClock |
| `InvalidHandle` | 7 | 无效句柄（V1 帧槽租约 / V2 流句柄） | V1+ |
| `NotInitialized` | 8 | 未 open 即查询（后端本地） | 后端 |
| `StreamNotFound` | 9 | 容器内无匹配流（无视频轨等） | V1 Demuxer |
| `OutOfMemory` | 10 | 内存不足 | V1 Decoder / 队列 |
| `QueueFull` | 11 | 帧队列满（非阻塞策略下） | V1 帧队列 |
| `Cancelled` | 12 | 操作被取消（stop/seek 中断 decode） | V1 Decoder |
| `Count` | 13 | **sentinel，只追加不重排** | — |

### 5.5 Enum discipline（normative）

- 所有枚举 `enum class` + 显式底层类型（`uint8_t`）+ 尾随 `Count` sentinel + `toString()` 全覆盖（switch 无 default 返回值兜底 "Unknown"）。
- 测试必须断言：`Ok == 0`、`sizeof(enum) == 1`、`Count == N`（防重排/防追加漏改）。

### 5.6 像素格式（V1 解码输出面）

| 格式 | 值 | 说明 |
|---|---|---|
| `Unknown` | 0 | 未初始化 |
| `I420` | 1 | YUV420 planar（ffmpeg 常见原始输出） |
| `NV12` | 2 | YUV420 semi-planar |
| `RGBA8` | 3 | 8-bit RGBA（Mock 输出 / 上屏友好） |
| `BGRA8` | 4 | 8-bit BGRA |
| `Count` | 5 | sentinel |

---

## 6. 核心数据结构

### 6.1 `MediaInfo`（POD + string）

```cpp
struct MediaInfo {
    int32_t width = 0, height = 0;
    double frameRate = 0.0;    // fps
    double durationSec = 0.0;
    bool hasVideo = false, hasAudio = false;
    std::string videoCodec, audioCodec;   // ffmpeg codec name（V1）
};
```

- 值语义，可整体赋值；`MediaInfo{}` 零初始化即"无媒体"（Null 后端语义）。
- `getMediaInfo` 输出副本，调用方无所有权。

### 6.2 `VideoPacket` / `VideoFrame`（POD 输出面）

```cpp
struct VideoPacket {
    const uint8_t* data = nullptr;   // demuxer 自有缓冲（§4.5）
    uint32_t size = 0;
    bool isVideo = true;
    int64_t streamIndex = 0;
    ayt::time::Duration pts, dts;
};

struct VideoFrame {
    const uint8_t* data = nullptr;   // decoder 自有缓冲（§4.5）
    uint32_t dataSize = 0;
    int32_t width = 0, height = 0;
    uint32_t stride = 0;
    VideoPixelFormat format = VideoPixelFormat::Unknown;
    ayt::time::Duration pts;
};
```

- **"no frame ready yet" 契约**：`dequeueFrame` 返回 `Ok` 且 `data == nullptr` = 当前无帧可取（不是错误不是 EOS）；`flush()` 之后同调用返回 `EndOfStream`（§8.3）。
- Mock 帧：320×240 RGBA8，`dataSize = 320×240×4`，`pts = 索引 × 40ms`。

### 6.3 帧队列（V1 normative，§8）

- **SPSC**（single-producer single-consumer）环形缓冲：生产者 = decode 线程，消费者 = player 线程（V1 期）→ 渲染线程（V3 期）。
- 元素：解码后 `VideoFrame`（拷贝像素数据，含格式换算 RGBA8 时）或引用计数槽；深度默认 4 帧（§16.3）。
- 满策略：V1 默认**阻塞生产者**（背压）；可选非阻塞 + `QueueFull`（§5.4 码 11）供 preview 场景。
- 空 = 消费者侧 `Ok + null` 同 §6.2 契约；队列不产生 EOS —— EOS 只来自 decoder flush 之后。

### 6.4 INV-nnn 不变式（V0.5 段，normative）

| # | 不变式 |
|---|---|
| INV-01 | `VideoResult::Ok == 0`；`Count` 恒为最大枚举值且只追加 |
| INV-02 | `VideoPacket.data == nullptr` 不得与 `Ok` 同现（readNextPacket 永远返回有效包或错误） |
| INV-03 | `dequeueFrame` 返回 `Ok + null` 只代表"暂无帧"，不得误判 EOS；EOS 仅在 flush 后 |
| INV-04 | `AYVideoPlayer` 拥有后端所有权；外部观察必须经 `demuxer()/decoder()` seam（防 moved-from 空指针 —— V0.5 实测 crash 教训，§17.1） |
| INV-05 | `open()` 仅 Idle/Stopped 合法；失败必须落 `Failed` 状态 + `lastResult` |
| INV-06 | 非法状态转换返回 `InvalidState` 且**状态不变**（transition 表 §10.4 单射执行） |
| INV-07 | `setRate` 出界拒绝且 `rate()` 保持原值（G-02 拒绝不钳制） |
| INV-08 | `stop()` 后必须关闭两个后端（`wasClosed` 可观测）且可重新 `open()` |
| INV-09 | SyncClock 未 `reset()`（无锚点）时 `position() == 0`（V0.5 实测 -2.1e9 教训，§17.1） |
| INV-10 | `markPaused` 冻结 position；`markResumed` 从冻结值续走（不跳变） |
| INV-11 | `setSource(AudioMaster)` 在未安装 `AudioMasterFn` 时必须返回 `InvalidState` 且 source 保持 EngineClock；安装 provider 后 Ok |
| INV-12 | Mock payload 确定性：`payload[0] == 包索引`、`pts == 索引 × 40'000 µs` |

---

## 7. Demux 设计

### 7.1 抽象（V0.5 已 ship）

```cpp
struct DemuxerOpenParams { std::string path; bool seekable = true; };

class IAYVideoDemuxer {
    virtual VideoResult open(const DemuxerOpenParams&) = 0;   // 已 open 再 open -> InvalidState
    virtual void close() noexcept = 0;
    virtual bool isOpen() const noexcept = 0;
    virtual VideoResult getMediaInfo(MediaInfo&) const = 0;   // open 后可用
    virtual VideoResult readNextPacket(VideoPacket&) = 0;     // 包缓冲属 demuxer（§4.5）
    virtual VideoResult seek(const ayt::time::Duration&) = 0; // 绝对定位；使旧包失效
};
```

### 7.2 后端语义（V0.5）

- **NullDemuxer**：`open` → Ok；`getMediaInfo` → 零值 MediaInfo；`readNextPacket` → 立返 `EndOfStream`（静默空流）。
- **MockDemuxer(n)`**：合成 320×240@25fps 视频轨；顺序吐 n 个包（payload 16 字节确定性 §6.4 INV-12）后 `EndOfStream`；`seek` 重置序列从头播（skeleton 契约）；`setFailOpen(true)` 故障注入 → open 返 `DemuxError`（供 Failed 状态机测试）。

### 7.3 V1 契约（normative for V1）

- ffmpeg：`avformat_open_input` / `avformat_find_stream_info` → 首个视频流 `AVStream`；`av_read_frame` 循环过滤音视频包；`avio_seek` 实现 seek（绝对秒 → `av_seek_frame`，V4 前只做关键帧级，帧精确定位 V4）。
- 容器支持 = ffmpeg 全支持；**V1 验证矩阵**：mp4 (h264+aac)、mkv (h264)、webm (vp8/vp9)。合成字节 stub：手写最小 MP4（`ftyp/moov/mdat` + 1 个 h264 关键帧 + 1 个 aac 帧）写死进测试资源，CI 无外部文件。
- 无视频轨 → `StreamNotFound`；容器损坏 → `DemuxError`（错误定位到具体 av 调用，诊断可查）。

---

## 8. Decode 设计

### 8.1 抽象（V0.5 已 ship）

```cpp
struct DecoderOpenParams { std::string codecName; MediaInfo media; bool decodeAudio = false; };

class IAYVideoDecoder {
    virtual VideoResult open(const DecoderOpenParams&) = 0;
    virtual void close() noexcept = 0;
    virtual bool isOpen() const noexcept = 0;
    virtual VideoResult feedPacket(const VideoPacket&) = 0;
    virtual VideoResult dequeueFrame(VideoFrame&) = 0;   // Ok+null = 暂无帧（§6.2）
    virtual VideoResult flush() = 0;                      // flush 后 dequeue -> EndOfStream
};
```

### 8.2 后端语义（V0.5）

- **NullDecoder**：`open` → Ok；`dequeueFrame` → Ok+null；`flush` 后 → `EndOfStream`。
- **MockDecoder(n)`**：`feedPacket` 记录 fed；`dequeueFrame` 在未 feed 前 Ok+null，之后顺序吐 n 帧（320×240 RGBA8，§6.4 INV-12），帧吐完未 flush 时 Ok+null；`flush` 后 `EndOfStream`；计数器 `openCount/feedCount/dequeueCount/flushCount/wasClosed` 供契约测试。

### 8.3 V1 线程模型（Q8 决策，normative for V1）

- **专用解码线程**（AYTask 管理，非 player 线程）：循环 `readNextPacket → feedPacket → dequeueFrame → 推入 SPSC 队列`。
- ffmpeg 解码器**非线程安全**：解码器实例单线程专属（§4.4），跨实例才可并行。
- flush 序列：seek/stop 时 `avcodec_flush_buffers` + 队列清空 + decoder `flush()`；EOS 判据 = demux EOS 且解码器 flush 完成。
- 取消：stop/seek 期间 decode 循环被标志位中断，进行中的解码返回 `Cancelled`（§5.4 码 12）。
- 时间戳：帧 `pts` 必须换算为统一时间基准（`av_pkt_rescale_ts` → 微秒 Duration），这是 A/V sync 输入面（§9）。

---

## 9. A/V Sync

### 9.1 SyncClock（V0.5 已 ship）

- `NowFn`（`TimePoint() noexcept`）注入式壁钟：测试 FakeNow，生产 `TimePoint::now()`。
- 引擎时钟路径：`position = mediaStart + (wallNow - anchorWall) × rate`（§6.4 INV-09/INV-10/INV-11）。
- `reset(mediaStart)` 锚定；`markPaused/markResumed` 冻结/续走；`setRate` 范围 [0.25, 4.0] 拒绝不钳制。

### 9.2 主从时钟（Q5a 决策，V2 normative）

- **audio-master**：音频渲染位置为主时钟（`master`），视频呈现为从（`slave`）。有音轨时同步源 = AYAudio 流位置（V2 桥，§11）；无音轨退化引擎时钟（EngineClock）。
- 漂移修正：视频帧呈现时 `slave_position - frame_pts` 超过容忍窗口（默认 ±40ms，V2 可配置）→ 丢帧（落后）或等待（超前）；音频永远不丢不补，只做启动对齐。
- `setSource(AudioMaster)` V2 前返回 `InvalidState`（§6.4 INV-11 已测）。

### 9.3 速率

- `setRate` [0.25, 4.0]；音频变速（resample）不在 V2 范围 —— V2 速率变化只作用于视频呈现 + 音频跳过策略，音频变速 V4 foresight。

---

## 10. 播放器状态机

### 10.1 状态集（V0.5 已 ship）

```
Idle → Opening → Ready → Playing ⇄ Paused → Stopped
                 ↘ Failed → Stopped（stop() 恢复）
```

| 状态 | 语义 |
|---|---|
| `Idle` | 构造完成，无媒体 |
| `Opening` | open() 在途（V1 异步 open 生效期） |
| `Ready` | 媒体打开，停在 0 位 |
| `Playing` | 呈现时钟运行 |
| `Paused` | 呈现时钟停止 |
| `Seeking` | seek 在途（V0.5 同步瞬时；V1 异步 dwell） |
| `Stopped` | stop() 已卸载媒体，可重新 open |
| `Failed` | 不可恢复错误，`lastResult()` 诊断 |

### 10.2 控制面（V0.5 已 ship）

`open(path)`（仅 Idle/Stopped）/ `play()` / `pause()` / `stop()` / `seek(target)` / `setLoop` / `setRate` / `state()` / `isPlaying()` / `getMediaInfo` / `lastResult()` / `demuxer()` + `decoder()` seam（§6.4 INV-04）。

### 10.3 转换表（V0.5 已 ship，§6.4 INV-05/INV-06）

| from \ to | Opening | Ready | Playing | Paused | Seeking | Stopped | Failed |
|---|---|---|---|---|---|---|---|
| **Idle** | ✓ | — | — | — | — | — | — |
| **Opening** | — | ✓ | — | — | — | — | ✓ |
| **Ready** | — | — | ✓ | ✓ | ✓ | ✓ | — |
| **Playing** | — | — | — | ✓ | ✓ | ✓ | — |
| **Paused** | — | — | ✓ | — | ✓ | ✓ | — |
| **Seeking** | — | ✓ | ✓ | ✓ | — | ✓ | ✓ |
| **Stopped** | ✓ | — | — | — | — | — | — |
| **Failed** | — | — | — | — | — | ✓ | — |

- 非法转换：返回 `InvalidState`，状态不变（单射执行，不产生副作用）。
- `pause()` 对 Ready/Paused/Seeking 幂等 Ok（skeleton 契约）；对 Idle/Opening/Stopped/Failed 返回 InvalidState。
- `seek()` 同步完成：`Seeking` 为瞬时 dwell，保存 preSeek 状态并恢复；V1 异步 seek 引入真实 dwell + 事件。

### 10.4 事件（V1 normative）

`onStateChanged(PlayerState)` / `onEndOfStream()` 注册回调（player thread 内同步触发，回调内禁止再入控制面 —— §4.4）。V0.5 不 ship（§2.6 最小必要公共面）。

---

## 11. 音频集成（V2 normative）

- AYAudio 桥（foresight 定位，V2 升格）：
  1. 解码线程解出音频帧（PCM F32，解码器侧重采样到 48kHz）→ F32 SPSC 音频队列。
  2. player 侧 `AudioEngine::openStream` 创建流 + `streamPush` 灌帧 + `playStream` 启动。
  3. audio-master：流位置查询（`streamGetPosition` 类 API 或播放计数）喂给 SyncClock `setSource(AudioMaster)`（§9.2）。
- 音频队列深度：0.5 s（§16.3）；溢出策略 V2 定（丢弃 vs 阻塞，倾向丢弃旧帧 —— 音频延迟最不可接受）。
- V0.5 **不引入**任何 AYAudio 头文件依赖（§2.6）。

---

## 12. 渲染集成预留（V3 normative）

- Q6a 决策：**帧纹理接口 + cross-module 新桥**，不走 AYResource `.ayvideo` 或 IMesh 路径。
- 形态（仿 VoxelChunkMesh:IMesh 先例的"接口 + 桥"二分）：
  - AYVideo 侧：`IVideoFrameTexture`（帧纹理抽象：`width/height/format` + `updateFromFrame(const VideoFrame&)` + 生命周期挂 `IAYVideoFrameSink`）。
  - AYRenderer 侧（V3）：纹理上传桥 —— 引擎纹理句柄接收 RGBA8 帧数据，`TextureUpdateQueue` 每帧批量提交（对齐 AYRenderer 现有 update 通道）。
- 呈现线程从 SPSC 队列取帧 → 格式换算（I420/NV12 → RGBA8，swscale，V3 范围内）→ `updateFromFrame` → 渲染。
- V3 demo 验收：AY2D_EngineDemo 式双路径截图对比（纹理存在性 + 帧内容校验）。

---

## 13. 资源集成（Q3a 决策）

- **独立并存**：AYVideo 不修改、不依赖 AYResource 的 `.ayvideo`（离线 cook 帧包，非流式）。两者定位不同：`.ayvideo` = 预烘帧序列（低延迟小体积场景），AYVideo = 实时流式解码。
- 未来（V6+ foresight）可加 `.ayvideo` 作为 `IAYVideoDemuxer` 的一个后端（零成本，接口已就位），**不承诺**。
- V0.5 无任何 AYResource 依赖。

---

## 14. 线程契约

- 总表见 §4.4；此处补充 V0.5 落地约束：
  - V0.5 全模块**单线程**（player thread）；无锁、无原子（`_paused`/`_anchored` 仅本线程可见）。
  - V1 起 decode 线程与 player 线程之间：SPSC 队列 + 原子取消标志 + 条件变量通知（AYTask），**禁止** player 持有解码器指针。
  - 测试并发：V1 起必须加 `Test_DecodeThread.cpp`（压力：400 帧循环 3 轮逐位一致，仿 AYAnimation P4 stress 先例）。

### 14.5 目录结构

```
AYRuntime/AYVideo/
├── design.md                  # 本文件（权威设计）
├── CLAUDE.md                  # 模块 AI 工作规则
├── README.md
├── .gitignore                 # build 产物；*.cmake 防重入 gotcha（!cmake/*.cmake）
├── CMakeLists.txt             # STATIC + cxx_std_20 + PUBLIC link AYTime + guard target + unittest gate
├── AYVideo.h                  # umbrella（聚合 8 个公共头/接口）
├── cmake/
│   └── CheckNoFFmpegInPublicHeaders.cmake   # G-01 硬门禁（14 pattern）
├── include/                   # 公共头（AY 前缀平铺，guard 扫描目录）
│   ├── AYVideoTypes.h         # VideoResult / VideoPixelFormat / toString
│   ├── AYVideoMediaInfo.h     # MediaInfo
│   ├── AYVideoFrame.h         # VideoPacket / VideoFrame
│   ├── AYVideoPlayer.h        # PlayerState / AYVideoPlayer
│   └── AYVideoSyncClock.h     # NowFn / SyncSource / AYVideoSyncClock
├── interface/                 # 后端抽象（guard 扫描目录）
│   ├── IAYVideoDemuxer.h
│   ├── IAYVideoDecoder.h
│   └── IAYVideoBackendFactory.h   # makeNull*/makeMock*（V1 加 makeFFmpeg*）
├── src/
│   ├── AYVideoTypes.cpp       # toString 实现
│   ├── AYVideoPlayer.cpp      # 状态机 + 控制面 + V1 播放管线
│   ├── AYVideoSyncClock.cpp   # 锚点/暂停/速率
│   ├── AYVideoBackendFactory.cpp
│   ├── FrameQueue.h           # V1 SPSC 帧环（§6.3，header-only）
│   └── DecodeLoop.h/.cpp      # V1 专用解码线程（std::thread，A-09）
├── backend/                   # 后端（不入公共 include path；测试相对 include）
│   ├── NullDemuxer.h/.cpp     # 静默空流
│   ├── NullDecoder.h/.cpp
│   ├── MockDemuxer.h/.cpp     # 合成 320×240@25fps + 故障注入
│   └── MockDecoder.h/.cpp
├── ecs/
│   └── VideoComponent.h       # POD 占位（V2+ VideoSubsystem 消费）
└── unittest/
    ├── main.cpp               # runAllTests("AYVideo")
    ├── Test_VideoTypes.cpp    # 结果码/格式 toString 全覆盖 + sentinel
    ├── Test_MediaInfo.cpp
    ├── Test_Frame.cpp
    ├── Test_NullBackends.cpp
    ├── Test_MockBackends.cpp
    ├── Test_PlayerState.cpp   # 状态机 + fixture（seam 观察）
    ├── Test_SyncClock.cpp     # FakeNow 确定性
    └── CMakeLists.txt
```

---

## 15. ECS 集成（V2+ foresight）

- **VideoSubsystem 推迟到 V2+**（Q9 决策）：不做 AYEntity 依赖、不做组件注册 —— 照抄 AYAudio `AudioSubSystem` 形态（V2 升格时核对实际 idiom）。
- `ecs/VideoComponent.h` 仅 POD 占位：`mediaPath / info / currentPosition / autoPlay / loop / volume` —— 不进入任何公共头，非 normative。
- V2 挂载点：`VideoSubSystem` tick 驱动 player（position 回写组件）+ 事件 → EventBus（对齐 AYAnimation AnimNotify 事件先例）。

---

## 16. 性能预算

| 项 | 预算 | 测量时机 |
|---|---|---|
| 解码线程 CPU | 单实例 ≤ 1 核（1080p30 h264 软解） | V1 |
| 帧队列深度 | 4 帧（解码）+ 0.5 s（音频） | V1/V2 |
| 解码→呈现延迟 | ≤ 120 ms（不含网络） | V1 |
| 内存（1080p 流） | ≤ 8 帧像素缓冲 ≈ 1080×1920×4×8 ≈ 66 MB | V1 |
| 队列满策略 | 阻塞背压；preview 可选 QueueFull | V1 |
| 同步容忍窗口 | ±40 ms（可配置） | V2 |
| 测试压力 | 400 帧 × 3 轮逐位一致 | V1 stress |

---

## 17. Phase ship checklist

### 17.1 V0.5（本次，已 ship）

- [x] design.md 全量工业级设计（§1–§21 + INV + 12 段 PR 模板附录）
- [x] `AYVideo` 独立 git repo（file:// 子模块注册）
- [x] `AYVideo.h` umbrella + 8 公共头/接口（零 ffmpeg 泄漏）
- [x] `CMakeLists.txt`（STATIC + cxx_std_20 + PUBLIC AYTime + guard target + unittest gate）
- [x] `cmake/CheckNoFFmpegInPublicHeaders.cmake` G-01 硬门禁
- [x] NullDemuxer / NullDecoder / MockDemuxer / MockDecoder（含故障注入）
- [x] AYVideoPlayer 状态机（§10.3 全表单射）+ SyncClock（§9.1）
- [x] 7 TEST_SUITE 194/194 PASS + ctest #38 绿
- [x] 根 CMakeLists `add_subdirectory(AYRuntime/AYVideo)` + option 文档
- [x] 构建 0 错（D9025 /W3→/W4 覆盖警告为既有兄弟模块同款，接受）
- [x] **构建期修复**：moved-from fixture 空指针 segfault（INV-04）+ SyncClock 无锚点 -2.1e9（INV-09）
- [ ] guard 双向验证（放入违规 include 触发 FAIL 再撤回）—— §19.3
- [ ] 模块内 commit + 根 pin commit

### V1 FFmpeg 最小播放（对齐 §7/§8）

- [x] vcpkg ffmpeg 引入（仅 src/backend/）+ guard 仍绿（`AYVIDEO_HAS_FFMPEG`；缺包时 Null/Mock 退化）
- [x] `FFmpegDemuxer` / `FFmpegDecoder`（§7.3/§8.3；平面打包进连续缓冲，防 FrameQueue span AV）
- [x] 专用解码线程（std::thread，A-09）+ SPSC 帧队列（§6.3）
- [x] 合成字节 stub（mpeg4+aac 生成器；A-04；AAC 合成轨用例暂 deferred V1.1）
- [x] 最小播放循环（EngineClock + `pullFrame`）+ 事件回调（§10.4）
- [x] `Test_DecodeThread.cpp` 压力 400×3 逐位一致
- [x] 0 错 + 全测试 **544/544 PASS** + 3-run stable
- [x] G-01 guard 双向验证（poison → FAIL → revert → PASS）

### V2 A/V Sync + 音频（对齐 §9.2/§11/§15）

- [x] audio-master 同步 + 漂移窗口（§9.2）— `setAudioMasterProvider` + `voicePositionFrames`；`pullFrame` ±40ms 丢帧/等待
- [x] AYAudio PCM 桥 + F32 SPSC 音频队列（§11）— `attachAudioEngine` / `AudioQueue` / FFmpeg 音频解码→swr→48k F32
- [x] 事件/控制面完整 + ECS VideoSubsystem 挂载（§15）— `VideoSubSystem` ISubSystem + `VideoComponent.playbackId`；实体 World 遍历仍后续 cross-module PR

### V3 渲染集成（对齐 §12）

- [x] `IVideoFrameTexture` + `IAYVideoFrameSink` + Cpu/Mock staging（无 AYRenderer PUBLIC 依赖）
- [x] 格式换算 I420/NV12/BGRA8→RGBA8（纯 C++ BT.601；`VideoColorConvert`）
- [x] `VideoSubSystem` present 路径：`setFrameTexture` / `setFrameSink`
- [x] AYRenderer `createDynamicTextureRgba8` / `updateTextureFromRgba8`（bgfx `updateTexture2D`；仿 FontAtlas）
- [x] AYVideo↔Renderer `IVideoFrameTexture` GPU 桥接（`RendererVideoFrameTexture` PRIVATE AYRenderer；`makeRendererVideoFrameTexture`；Noop UT）
- [x] 引擎 demo 上屏验收（`AYVideo_EngineDemo`：双路径 solid/I420 + frame 30/60 截图 + `check_screenshot.ps1`）

### V4+（对齐 §3 表）

- [x] seek 帧精确（slice-1）：keyframe seek + `_minPresentPts` 丢弃 + pause→seek→play 管线重启；CFR ±1 帧 UT
- [x] 错误恢复 / 丢帧（slice-2）：中途 `DemuxError`/`DecodeError` soft-skip，保持 Playing；Mock inject UT
- [x] 字幕轨 discovery（slice-3）：`SubtitleTrackInfo` / player 选择 API；Mock + FFmpeg 枚举；无 cue 渲染
- [x] 多轨选择（slice-4）：`Video/AudioTrackInfo` + deferred `setActive*`（play/seek 应用）；Mock 双音轨 UT
- [ ] 内存压力 / 双向 seek 精修

---

## 18. 反模式表

| # | 反模式 | 后果 | 正确做法 |
|---|---|---|---|
| A-01 | 公共头 `#include <libavformat/...>` | 全引擎公共面污染 + guard FAIL | 接口消化，ffmpeg 限 src/backend/（G-01） |
| A-02 | `void` 失败路径（`open` 不返回码） | 调用方无法区分失败原因 | 全路径返回 VideoResult + lastResult（G-02） |
| A-03 | 错误码重排/插入中间 | 序列化/匹配表漂移，存量代码误判 | 只追加 + Count sentinel（§5.4） |
| A-04 | 测试持有 moved-from unique_ptr 再解引用 | **空指针 segfault（V0.5 实测）** | 观察走 player seam（INV-04） |
| A-05 | 双后端只测一个 | Noop 绿 ≠ FFmpeg 能编译（AYRenderer 双后端盲区教训） | 每次后端改动双后端编译 + 测试 |
| A-06 | 测试依赖真实壁钟/外部媒体文件 | CI 抖动/不可复现 | FakeNow + 合成字节（G-04） |
| A-07 | 解码器跨线程使用 | ffmpeg 解码器非线程安全 → 随机崩溃 | 单实例单线程 + SPSC（§4.4/§8.3） |
| A-08 | seek 直接 `av_seek_frame` 不 flush | 脏解码器状态 → 花屏/崩溃 | flush 序列（§8.3） |
| A-09 | 帧 pts 不统一时间基准 | A/V sync 错乱 | `av_pkt_rescale_ts` → µs Duration（§8.3） |
| A-10 | 状态机非法转换带副作用 | 半进入错误状态 | 转换表单射执行，非法返回且不变（INV-06） |
| A-11 | rate 越界静默 clamp | 调用方不知道被改 | 拒绝 + 保持原值（INV-07） |
| A-12 | 事件回调内调用控制面 | 重入 → 死锁/状态撕裂 | 回调内禁止（§10.4） |
| A-13 | 阻塞式 `readNextPacket` 在主线程 | UI 卡顿 | 解码线程专职（§8.3） |
| A-14 | 枚举无 sentinel 追加 | 忘改 switch → 漏码 | Count + toString 全覆盖测试（§5.5） |

---

## 18.5 新增模块要求

### 新增 Backend（Demuxer / Decoder）

1. `backend/XxxDemuxer.h/.cpp`（或 `XxxDecoder`），实现 `IAYVideoDemuxer` / `IAYVideoDecoder`。
2. 公共头零泄漏（G-01）：第三方 include 只在 `.cpp`，若必须进头 → PIMPL。
3. `IAYVideoBackendFactory` 加 `makeXxx*` + `src/AYVideoBackendFactory.cpp` 实现。
4. `unittest/Test_XxxBackends.cpp` 覆盖：open 契约 / 序列 / EOS / flush / close 标记 / 故障注入（新后端需加 `setFailOpen` 类注入）。
5. `design.md §3` 路线图 + §17 checklist 加 ship step。

### 新增公共 API

1. 头文件先立契约注释（§x 引用 design.md 章节），测试先写（红）再实现（绿）。
2. 破坏性改动必须走 §20 amendment。
3. 枚举/结果码只追加（§5.4/§5.5）。

---

## 19. 验证方式

### 19.1 构建命令（本机惯例）

```bat
rem %temp%\build_ayvideo_v05.bat —— vcvars64.bat -arch=x64 后 pushd 构建目录
cmake --build D:\Projects\out\build\x64-Debug --target AYVideo_Tests
```

- **VsDevCmd.bat -arch=x64**（非 vcvars64 直接调用）；ninja 在 VS env 才在 PATH；`cmd /c` 内联 && 链在 bash 不可靠 → 用 .bat 文件 + cygpath。

### 19.2 测试门槛

- `AYVideo_Tests.exe` 全 PASS（当前 194/194）+ `ctest -R AYVideo --output-on-failure` 绿。
- **stdout 重定向全缓冲 landmine**：`exe > file` 时 stdout 全缓冲，崩点输出会丢失尾部 —— 判断崩溃位置必须看"最后 flush 的 4KB 块"，或用管道实时输出；本轮 segfault 排查被此掩盖一次（§17.1）。
- 3-run stable：每次改动后连跑 3 次全绿才可 ship（AYAnimation 纪律）。

### 19.3 Guard 双向验证（G-01）

1. `ninja ayvideo_check_no_ffmpeg_in_public_headers` 当前绿。
2. 临时在 `include/AYVideoTypes.h` 插入 `#include <libavcodec/avcodec.h>` → target FAIL（14 pattern 命中）。
3. 撤回 → 恢复绿。

---

## 20. Normative amendments

| # | 日期 | 变更 | 影响 |
|---|---|---|---|
| A-01 | 2026-08-13 | V0.5 定稿：Q1a ffmpeg / Q2a stub-only / Q3a 独立并存 / Q4a 全格式 V1 矩阵 / Q5a audio-master / Q6a 帧纹理桥 / Q7a 合成字节 | §3 路线图 §12 §13 |
| A-02 | 2026-08-13 | INV-04 修正（V0.5 实测 crash）：player 增加 `demuxer()/decoder()` seam；测试 fixture 禁持 moved-from 指针 | §6.4 §17.1 |
| A-03 | 2026-08-13 | INV-09 修正（V0.5 实测 -2.1e9）：SyncClock 未锚定 → position 0 | §6.4 §9.1 |
| A-04 | 2026-08-14 | **V1 验证矩阵替换**：vcpkg ffmpeg 构建禁用 libx264/libopenh264 → 无 h264 编码器；合成字节 stub 改用**原生 mpeg4 + aac**（两者内置、确定性、CI 稳定）。h264 解码仍受支持（矩阵只测本机可生成的编码），真实 h264 样本留 V4+ | §7.3 §17 |
| A-05 | 2026-08-14 | **V1 帧契约修正**：`VideoFrame.dataSize` 语义改为**全平面总字节** + 新增 `planeOffset[3]`（V3 swscale 需要全平面；原实现只报 plane 0 → U/V 静默丢失）。I420/NV12 平面偏移从实际指针差计算（含对齐 padding）。单平面格式 offset 全 0 | §6.2 §8.3 |
| A-06 | 2026-08-14 | **§10.3 转换表补充（V1 管线现实）**：新增 `Playing → Ready`（EOS，非 loop）与 `Playing → Failed`（解码错误）。EOS 语义：事件一次 → state Ready（position 停在末尾，Ready 描述修正为"停在 0 位或 EOS 后末尾"）；loop 模式静默重启（seek 0 + flush + 新 decode 循环，无事件无状态变更）。`play()` 从 Ready = 从 0 重播（含 EOS 后）；从 Paused 恢复冻结位置（若 pause 期间流已解完则 seek 回冻结位置重启） | §10.3 §10.4 |
| A-07 | 2026-08-14 | **V1 呈现原语**：`pullFrame(VideoFrame&)` 为最小播放循环的呈现接口（V3 渲染前测试/上层唯一取帧路径）：仅 Playing 合法；时钟门控 —— 队头帧 pts ≤ clock.position() 才返回，未到期帧暂存于单帧 hold（先进先出，不跳帧不重排）；空队 = Ok+null（§6.2 语义）；EOS/错误见 A-06。帧数据所有权 = player（有效至下一次 pullFrame/seek/stop，§4.5 同型）。事件回调（`setOnStateChanged`/`setOnEndOfStream`）在调用线程同步触发，禁止回调内再入（A-12） | §10.4 |
| A-08 | 2026-08-14 | **V1 时间基准（CFR 假设）**：FFmpegDecoder 打开时从 `media.frameRate` 推导 codec time_base = 1/fps（open 前后各设一次，防 codec 覆盖）；feed/dequeue 的 µs⇄tb 往返一致。VFR/多轨精确时间基准留 V4+（V1 验证矩阵全 CFR） | §8.3 §9 |
| A-09 | 2026-08-14 | **Q8 偏差落地**：解码线程用 **std::thread** 而非 AYTask —— AYTask 是短任务 job pool（submit/wait/waitAll，无持久线程语义），不满足"专用解码线程生命周期 = 播放期"；§8.3/§14/§14.5 的"AYTask 管理"字样以本条为准。取消 = 原子标志 + 队列 clear 解阻塞 + join（§8.3 取消序列） | §8.3 §14 |
| A-10 | 2026-08-14 | **V1 open() 同步化**：Opening 为瞬时 dwell（本地文件 open 毫秒级）；异步 open（后台线程 + 真实 dwell）不承诺，留 V2+ | §10.1 §10.3 |
| A-11 | 2026-08-14 | **无 ffmpeg 构建**：`makeFFmpeg*()` 返回 nullptr（`AYVIDEO_HAS_FFMPEG` 编译定义门控）；player `open()` 对空后端返回 `UnsupportedFormat` → Failed。ffmpeg 缺失时模块退化为 Null/Mock 纯测试面 | §7.3 §18.5 |
| A-12 | 2026-08-14 | **MockDecoder flush 语义对齐 §8.3**：flush() 重置 `_emitted`（seek 后新包必须能再产出帧）；V1 解码线程在 demux EOS 后调 decoder.flush() 排空。`feedPacket` 在真实数据包时清除 drain 标志（flush 后 feed 不再误报 EOS） | §8.2 §8.3 |
| A-13 | 2026-08-14 | **V1 ship 落地修正**：① CMake 用 `FFMPEG_*` 变量 + 本地 `AYVideo_FFmpeg` INTERFACE（vcpkg FindFFMPEG 不创建 `FFmpeg::*` targets）；② `FrameQueue` 用 `unique_ptr<Slot[]>`（Slot 含 atomic，不可 `vector::resize`）；③ MSVC 禁 `av_err2str` compound literal → `av_strerror`；④ FFmpegDecoder 打开时注入 width/height/extradata；⑤ `feedPacket` 对 EAGAIN 返 `QueueFull` + DecodeLoop 重试；⑥ `flush()` = `avcodec_flush_buffers`（seek/replay），EOS drain 用 null `feedPacket`；⑦ **平面打包**：dequeue 将 I420/NV12 逐平面拷进连续 `packed` 缓冲（AVFrame 平面地址递增但中间可有未映射空洞 → FrameQueue `assign` span AV，0xC0000005）；⑧ 解码时间基锚定 µs（避开 find_stream_info 把 25fps 估成 27） | §6.3 §7.3 §8.3 §17 |
| A-14 | 2026-08-14 | **V1.1 deferred**：AAC 合成轨用例（`makeClip(true)`）在本机 FFmpeg 8 构建下曾触发后续套件不稳定，暂跳过；loop 全时间线重启 / pause-resume 深路径 / seek-while-playing 深路径保留 smoke，完整覆盖回补 V1.1 | §17 §19 |

---

## 21. Changelog

- **2026-08-14 (V4 multi-track slice-4)**：`AYVideoTrack` + MediaInfo 音视频轨列表；player deferred `setActive*`；FFmpeg/Mock `setActiveStreamIndices`。
- **2026-08-14 (V4 subtitle slice-3)**：`AYVideoSubtitle` + MediaInfo 轨列表；player `setActiveSubtitleTrack`（选择-only）；Mock/FFmpeg 枚举；无 cue 渲染。
- **2026-08-14 (V4 recovery slice-2)**：DecodeLoop 对中途 DemuxError/DecodeError soft-skip；Mock `failReadAt`/`failFeedAt`；`Test_ErrorRecovery`。
- **2026-08-14 (V4 seek slice-1)**：`seek` 设 `_minPresentPts` 丢弃预目标帧；Paused 无 decode loop 时 `play()` 按 clock 位权重启；`PlayerSeekLandsNearTargetPts`。
- **2026-08-14 (V3 demo)**：`AYVideo_EngineDemo`（GameLoop + RendererSubSystem + textured quad）；`AYVIDEO_DEMO_PATH=1|2` solid/I420；`check_screenshot.ps1`。
- **2026-08-14 (V3 GPU bridge)**：`RendererVideoFrameTexture` PRIVATE 链 AYRenderer；`makeRendererVideoFrameTexture` + `gpuTextureId()`；Noop UT。
- **2026-08-14 (V3 Renderer)**：AYRenderer 落地 `createDynamicTextureRgba8` + `updateTextureFromRgba8`（动态 RGBA8 + `bgfx::updateTexture2D`）。GPU `IVideoFrameTexture` 桥 + demo 仍待。
- **2026-08-14 (V3 start)**：`IVideoFrameTexture` / `IAYVideoFrameSink` + Cpu/Mock staging + BT.601 `VideoColorConvert`；`VideoSubSystem` 绑纹理/sink。AYRenderer 动态纹理 update API 仍缺，demo 上屏待后续。
- **2026-08-14 (V2+)**：`VideoSubSystem`（ISubSystem priority 650，依赖 Audio）+ `player.position()` + `VideoComponent.playbackId`。
- **2026-08-14 (V2 start)**：AudioMaster provider + drift 窗口；AYAudio `voicePositionFrames`；FFmpeg 可选音频解码 + AudioQueue + `attachAudioEngine` PCM 桥。ECS VideoSubsystem 仍 V2+。
- **2026-08-14 (V1)**：FFmpeg 后端 + DecodeLoop + FrameQueue + `pullFrame` + 事件；544/544 × 3-run；guard 双向验证；A-13/A-14 落地修正与 deferred。
- **2026-08-13 (V0.5)**：模块创建。公共面 8 头 + 4 后端 + Player 状态机 + SyncClock stub；194/194 PASS。2 个构建期修复：moved-from fixture 空指针 segfault（加 seam + fixture 重构）；SyncClock 无锚点垃圾 position（`_anchored` 门）。guard 硬门禁就位。

---

## 附录 A：12 段 PR 模板（AYAnimation 纪律）

每个 Phase ship PR 必须包含：

1. **背景与目标**（引用 design.md §x 的 Phase 行）
2. **决策**（Q 编号决策 + 替代方案 + 为何选）
3. **接口变更**（新增/删除/破坏性，含 §20 amendment 编号）
4. **实现摘要**（文件级：新增/修改/删除）
5. **不变式核对**（INV-nnn 逐条：新增/受影响/未受影响）
6. **测试**（新增 case 数 / 修改 / 删除 + 覆盖点）
7. **性能**（§16 预算逐项实测或声明未涉及）
8. **构建**（0 错 0 警 + 警告明细）
9. **回归**（AYVideo 全 PASS + 兄弟模块数字不变：AYTime / AYTest 等）
10. **3-run stable**（连续 3 次全绿数字）
11. **教训 / landmine**（本次踩坑记录，进本文件对应章节）
12. **deferred**（本轮明确不做、留给下一 Phase 的清单）
