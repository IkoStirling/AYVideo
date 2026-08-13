# AYVideo 项目 AI 工作注意事项

> **注意**：AYVideo 是独立子模块，遵循本文件定义的规则。AYTest 是独立测试框架库，位于 `AYFoundation/AYTest/CLAUDE.md`。
> **权威设计**：[`design.md`](design.md)（V0.5 ship 2026-08-13，§1–§21 + INV-nnn + 12 段 PR 模板附录）。代码与 design.md 不一致时，design.md 优先。

## 当前状态 — V0.5 stub-only skeleton ship (2026-08-13)

- **已 ship**：公共面 8 头（`AYVideoTypes` / `AYVideoMediaInfo` / `AYVideoFrame` / `AYVideoPlayer` / `AYVideoSyncClock` + `IAYVideoDemuxer` / `IAYVideoDecoder` / `IAYVideoBackendFactory`）+ Null/Mock 双后端 + Player 状态机（§10.3 全表单射）+ SyncClock stub（§9.1）+ guard 硬门禁（§2.1 G-01）+ 7 TEST_SUITE **194/194 PASS** + ctest #38 绿。零第三方依赖。
- **锁行为**（design.md §5.4/§5.5/§6.4/§10.3/§9.1 全部生效）：
  - `VideoResult::Ok = 0`，13 个错误码 + `Count = 13` sentinel，只追加不重排
  - `VideoPixelFormat`：Unknown/I420/NV12/RGBA8/BGRA8 + `Count = 5`
  - `PlayerState`：Idle/Opening/Ready/Playing/Paused/Seeking/Stopped/Failed（Seeking 为 V0.5 瞬时 dwell）
  - `SyncSource`：EngineClock/AudioMaster（AudioMaster 到 V2 才可用，设了返 `InvalidState`）
  - 所有 public mutator 返回 `VideoResult`；**禁止** `void` 失败路径（§2.2 G-02）
  - 公开头零 ffmpeg（§2.1 G-01）
  - `setRate` 范围 [0.25, 4.0] **拒绝不钳制**（§6.4 INV-07）
  - `AYVideoPlayer::demuxer()/decoder()` seam 是外部观察后端的唯一途径（§6.4 INV-04）
  - SyncClock 未 `reset()` 时 `position() == 0`（§6.4 INV-09）
- **next direction = V1 FFmpeg 最小播放**（design.md §3 V1 行）：vcpkg ffmpeg demuxer/decoder 后端 + AYTask 专用解码线程 + SPSC 帧队列 + 合成字节 stub（手写最小 MP4）+ 最小播放循环。V1 起 ffmpeg 只允许进 `src/backend/`。
- pending cross-module PRs（design.md §4.2 deferred to V2+）：
  - **AYAudio**: PCM 桥（`openStream/streamPush/playStream`）—— V2 A/V sync
  - **AYRenderer**: `IVideoFrameTexture` 纹理上传桥 —— V3 上屏
  - **AYEntity**: `VideoSubsystem` ECS 挂载 —— V2+

## 重要规则

1. **UTF-8 only，禁止 GBK 中文注释** — 与其它 sibling 模块一致；本仓库 `design.md` 中文为合法文档内容，但 `.h / .cpp / .cmake` 不应有 GBK。
2. **代码与 design.md 不一致时，design.md 优先** — 修改设计须先改 design.md §20 amendments + §21 Changelog，再动代码。
3. **V0.5 禁止**：
   - 写 `avformat/avcodec/avutil` 等 ffmpeg 调用（公共头严格零 ffmpeg；ffmpeg 只允许 V1 起在 `src/backend/` .cpp 内）
   - 触碰其它模块源码（cross-module PR 走对应模块，design.md §4.2）
   - 实现真实 demux/decode / A/V sync / 渲染上屏 — 这些是 V1+ 范围
4. **公开头零 ffmpeg** — `include/*.h` + `interface/*.h` 不得 include `<libavformat/*>` / `<libavcodec/*>` / `<libavutil/*>` 等 14 个 forbidden pattern。V0.5 CI 通过 `ayvideo_check_no_ffmpeg_in_public_headers` target 强制（G-01 / A-01）。
5. **可主动构建 + 跑测试** — 已 OK。`%temp%\build_ayvideo_v05.bat`（vcvars64.bat + pushd 构建目录 + `cmake --build . --target AYVideo_Tests`）跑通；`ctest -R AYVideo --output-on-failure` 绿。

## 踩坑速查（V0.5 landmines）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| **#1** | 测试运行到 PlayerStateSuite 附近 segfault (exit 139)，只有 5/7 SUITE 有输出 | fixture 把 `unique_ptr<MockDemuxer>` move 进 player 后自身成员变 null，`fx.demuxer->seekCount()` 等 7 处空指针解引用 | 观察走 `player.demuxer()/decoder()` seam（§6.4 INV-04）；**禁止**测试持 moved-from 指针 |
| **#2** | stdout 重定向后崩点难定位 | `exe > file` 时 stdout **全缓冲**，崩时尾部输出丢失，最后一行只是"最后 flush 的 4KB 块"不是崩点 | 判崩点看 flush 块边界；或管道实时输出（§19.2） |
| **#3** | `clk.position()` 默认构造返回 -2.1e9 µs | 未 `reset()` 时 `wallNow() - TimePoint{}` = 自纪元起真实流逝 | `_anchored` 标志门，未锚定返回 0（§6.4 INV-09） |
| **#4** | CHECK_FLOAT_EQ 传 double 出现 C4244/C4305 | 宏内部用 float | `static_cast<float>(x)` + `1e-9f` 后缀 |
| **#5** | 测试 include `MockDemuxer.h` C1083 | backend/ 不在测试 include path | 相对 include `"../backend/Xxx.h"`（§5.2） |
| **#6** | D9025 warning（/W3 overriding /W4） | 根 CMakeLists /W3 与子模块 /W4 既有模式 | 兄弟模块同款，接受（§17.1） |

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
