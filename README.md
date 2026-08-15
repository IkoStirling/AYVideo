# AYVideo

流式实时视频播放模块（解封装 → 解码 → 帧队列 → 呈现/音频消费）。工业级设计：结果码纪律、线程契约、确定性测试、公开头零第三方泄漏。

**当前状态**：V1 FFmpeg 最小播放（2026-08-14）— FFmpeg demux/decode + 解码线程 + SPSC 帧队列 + `pullFrame` 时钟门控，**544/544 PASS × 3-run stable**。无 ffmpeg 时退化为 Null/Mock 测试面。

## 快速开始

```cpp
#include <AYVideo.h>

using namespace ayt::video;

AYVideoPlayer player(makeFFmpegDemuxer(), makeFFmpegDecoder());

if (player.open("clip.mp4") == VideoResult::Ok) {
    player.play();
    VideoFrame frame;
    while (player.pullFrame(frame) == VideoResult::Ok) {
        if (frame.data) {
            // present frame (pts-gated by EngineClock)
        }
    }
}
player.stop();
```

## 特性

- **三后端**：Null / Mock / FFmpeg（`IAYVideoDemuxer` + `IAYVideoDecoder`），工厂注入
- **解码线程 + SPSC 帧队列**：背压阻塞；`QueueFull` 可重试
- **`pullFrame`**：时钟门控呈现（V3 渲染前的唯一取帧路径）
- **结果码纪律**：`VideoResult` 13 码 + `Count` sentinel
- **公开头零 ffmpeg**：CMake guard 硬门禁（含双向验证）
- **确定性测试**：合成 mpeg4 + FakeNow，CI 无外部文件

## 路线图

| Phase | 内容 | 状态 |
|---|---|---|
| V0.5 | 骨架 + Null/Mock + 状态机 + 测试 | ✅ 2026-08-13 |
| V1 | FFmpeg demux/decode + 解码线程 + SPSC + 最小播放 | ✅ 2026-08-14 |
| V2 | audio-master A/V sync + AYAudio PCM 桥 + ECS | ✅ |
| V3 | AYRenderer 帧纹理桥 + 上屏 | ✅ |
| V4+ | seek / 字幕 / scrub 契约 / setRate↔timeScale | ✅（soft cues；FFmpeg cue 解码仍后） |
| V5 | 网络流 HTTP(S) / RTSP / HLS | ✅（ABR=preferred bandwidth；真机验收可选） |
| V6+ | 硬解 / 播放列表 / … | 硬解 API+软回退 ✅；零拷贝仍后 |

详见 [design.md](design.md) §3。

## 构建与测试

```bat
d:\Projects\do_cmake.bat --build d:\Projects\out\build\x64-Debug --target AYVideo_Tests -j 8
ctest -R AYVideo --output-on-failure
```

需要 vcpkg `ffmpeg`（`AYVIDEO_HAS_FFMPEG=1`）。缺包时仅编 Null/Mock。

## 目录结构

```
include/   公共头（AY 前缀）
interface/ 后端抽象（IAY 前缀）
src/       实现（Player / SyncClock / DecodeLoop / FrameQueue）
backend/   Null/Mock/FFmpeg 后端
ecs/       VideoComponent 占位（V2+）
unittest/  测试（一文件一 TU）
```

详见 [design.md](design.md) §14.5。
