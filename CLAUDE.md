# AYVideo 项目 AI 工作注意事项

> **注意**：AYVideo 是独立子模块，遵循本文件定义的规则。AYTest 是独立测试框架库，位于 `AYFoundation/AYTest/CLAUDE.md`。
> **权威设计**：[`design.md`](design.md)（V0.5 ship 2026-08-13，§1–§21 + INV-nnn + 12 段 PR 模板附录）。代码与 design.md 不一致时，design.md 优先。

## 当前状态 — V1 FFmpeg 最小播放 ship (2026-08-14)

- **已 ship**：FFmpeg demux/decode + DecodeLoop（std::thread）+ SPSC FrameQueue + `pullFrame` + 事件回调 + 合成 mpeg4 stub + **544/544 PASS × 3-run** + G-01 guard 双向验证。无 ffmpeg 时 Null/Mock 退化。
- **V0.5 锁行为仍生效**（见下）；V1 额外：平面打包连续缓冲（防 FrameQueue span AV）、`QueueFull` 重试、`flush`=`avcodec_flush_buffers`、时间基锚定 µs。
- **next direction = V2 A/V Sync + 音频**（design.md §3 V2 行）：audio-master + AYAudio PCM 桥 + ECS VideoSubsystem。
- **V1.1 deferred**（A-14）：AAC 合成轨用例、loop 全时间线重启、pause-resume / seek-while-playing 深路径。
- pending cross-module PRs（design.md §4.2 deferred to V2+）：
    - **AYAudio**: PCM 桥（`openStream/streamPush/playStream`）—— V2 A/V sync
    - **AYRenderer**: `IVideoFrameTexture` 纹理上传桥 —— V3 上屏
    - **AYEntity**: `VideoSubsystem` ECS 挂载 —— V2+

## 重要规则

1. **UTF-8 only，禁止 GBK 中文注释** — 与其它 sibling 模块一致；本仓库 `design.md` 中文为合法文档内容，但 `.h / .cpp / .cmake` 不应有 GBK。
2. **代码与 design.md 不一致时，design.md 优先** — 修改设计须先改 design.md §20 amendments + §21 Changelog，再动代码。
3. **公开头零 ffmpeg** — `include/*.h` + `interface/*.h` 不得 include libav*；ffmpeg 只在 `backend/*.cpp`。
4. **可主动构建 + 跑测试** — `do_cmake.bat --build ... --target AYVideo_Tests`；3-run stable 才可 ship。
5. **FrameQueue 只 memcpy 连续 span** — FFmpegDecoder 必须把平面打包进 `packed`；禁止把 AVFrame 非连续平面指针交给队列。

## 踩坑速查（V0.5 + V1 landmines）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| **#1** | 测试 fixture segfault | moved-from unique_ptr | 经 `demuxer()/decoder()` seam 观察（INV-04） |
| **#2** | stdout 重定向崩点难定位 | 全缓冲 | 管道实时输出 / 看 flush 块 |
| **#3** | SyncClock position -2.1e9 | 未锚定 | `_anchored` 门（INV-09） |
| **#7** | `FrameQueue::push` 0xC0000005 | AVFrame 平面地址递增但中间有未映射空洞，`dataSize` 跨空洞 | decoder 逐平面打包进连续 `packed`（A-13） |
| **#8** | vcpkg FindFFMPEG 找不到后端 | 不创建 `FFmpeg::*` targets | 用 `FFMPEG_*` 变量 + INTERFACE wrapper |
| **#9** | play() 后首帧 DecodeError | `flush()` 误发 nullptr 进 drain | `flush`=`avcodec_flush_buffers`；EOS 用 null feed |

## 命名约定（与 sibling 一致）

- 子模块 / 目录：`AYRuntime/AYVideo`。
- 命名空间：`ayt::video`（与 `ayt::physics` / `ayt::entity` / `ayt::ay2d` 同级）。
- 公共类名**不带** `AY` 前缀（`MediaInfo` / `VideoFrame` / `AYVideoPlayer` / `NullDemuxer`）——门面类 `AYVideoPlayer` / `AYVideoSyncClock` 例外带前缀（对齐公共头文件前缀惯例）。
- 公共头文件名带 `AY` 前缀（`AYVideoTypes.h` / `AYVideoPlayer.h`）；接口文件名用 `I` 前缀（`IAYVideoDemuxer.h`）。
- 私有成员 `_` 前缀 camelCase（`_state` / `_demuxer`）；公共成员 plain camelCase。
- 测试：`unittest/Test_<Subject>.cpp` 一文件一 TU；用 AYTest 的 `TEST_SUITE` / `TEST_CASE` / `CHECK_*`。

## 反模式（与 `design.md` §18 同步）

| 反模式 | 替代 |
|---|---|
| 在 `include/*.h` / `interface/*.h` 引入 ffmpeg 头 | 公开头零 ffmpeg；ffmpeg 仅在 `src/backend/` .cpp 内（G-01） |
| `void` 失败路径 | 返回 `VideoResult` + `lastResult()` 诊断（G-02） |
| 错误码中间插入 / 重排 | 只追加 + `Count` sentinel（§5.4） |
| 测试持 moved-from unique_ptr | 经 `player.demuxer()/decoder()` seam 观察（INV-04 / landmine #1） |
| 解码器跨线程使用 | 单实例单线程 + SPSC 队列（§4.4 / §8.3） |
| 测试依赖真实壁钟 / 外部媒体文件 | FakeNow + 合成字节 stub（G-04） |
| rate 越界静默 clamp | 拒绝 + 保持原值（INV-07） |
| seek 不 flush 解码器 | V1 flush 序列（§8.3 A-08） |
| 事件回调内调控制面 | 回调内禁止重入（§10.4 A-12） |
| 双后端只测一个 | 每次后端改动双后端编译 + 测试（G-01 双后端盲区 / A-05） |

## 引用

- [design.md](design.md) — 权威设计（V0.5 ship；§3 phase roadmap + §6.4 INV + §10.3 转换表 + §14.5 layout + §17 ship checklist + §18.5 new-module rules + §19 verify）。
- 兄弟模块设计：`d:/Projects/AYRuntime/AYVoxel/design.md`（骨架模板）、`AYAnimation/design.md`（INV + 12 段 PR 模板纪律）、`AYAudio/`（媒体库 vcpkg + guard 形态）。
- 根目录 docs：`d:/Projects/ENGINE-FOUNDATION-PLAN.md`、`ENGINE-DETERMINISM-ARCHITECTURE.md`、`AYRuntime/docs/first-game-engine-capability-map.md`。
