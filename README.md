# AYVideo

流式实时视频播放模块（解封装 → 解码 → 帧队列 → 呈现/音频消费）。工业级设计：结果码纪律、线程契约、确定性测试、公开头零第三方泄漏。

**当前状态**：V0.5 stub-only skeleton（2026-08-13）— 接口面 + Null/Mock 双后端 + 播放器状态机 + SyncClock stub，194/194 测试 PASS，零第三方依赖。真实 FFmpeg 解码为 V1。

## 快速开始

```cpp
#include <AYVideo.h>

using namespace ayt::video;

// 默认 Null 后端（无操作占位）；V1 起可换 FFmpeg 后端
AYVideoPlayer player;

if (player.open("clip.mp4") == VideoResult::Ok) {
    player.play();
    MediaInfo info;
    player.getMediaInfo(info);   // 元数据（宽高/fps/时长/编码）
}
player.stop();
```

## 特性

- **双 seam 后端**：`IAYVideoDemuxer`（容器解析）+ `IAYVideoDecoder`（编解码），工厂注入，Null / Mock / FFmpeg（V1）可切换
- **结果码纪律**：`VideoResult` 13 码 + `Count` sentinel，拒绝不钳制（`setRate` [0.25, 4.0]）
- **状态机**：Idle → Opening → Ready → Playing ⇄ Paused → Stopped，Failed 可恢复，非法转换 `InvalidState` 且状态不变
- **SyncClock**：可注入壁钟的播放时钟（锚定 / 暂停冻结 / 速率），audio-master 同步 V2
- **公开头零 ffmpeg**：CMake guard 硬门禁
- **确定性测试**：合成字节 + FakeNow，CI 无外部文件

## 路线图

| Phase | 内容 | 状态 |
|---|---|---|
| V0.5 | 骨架 + Null/Mock + 状态机 + 测试 | ✅ 2026-08-13 |
| V1 | FFmpeg demux/decode + 解码线程 + SPSC 帧队列 + 最小播放 | 进行中 |
| V2 | audio-master A/V sync + AYAudio PCM 桥 + ECS | — |
| V3 | AYRenderer 帧纹理桥 + 上屏 | — |
| V4+ | seek 精修 / 字幕 / 网络流 | — |

详见 [design.md](design.md) §3。

## 构建与测试

```bat
rem %temp%\build_ayvideo_v05.bat（vcvars64.bat -arch=x64 + cmake --build --target AYVideo_Tests）
ctest -R AYVideo --output-on-failure
```

## 目录结构

```
include/   公共头（AY 前缀）
interface/ 后端抽象（IAY 前缀）
src/       实现
backend/   Null/Mock 后端
ecs/       VideoComponent 占位（V2+）
unittest/  测试（一文件一 TU）
```

详见 [design.md](design.md) §14.5。
