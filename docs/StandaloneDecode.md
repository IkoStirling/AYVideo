# Standalone CPU decode

**Date:**2026-10-25 · **Status:**Experimental · **Owner:**AYVideo

Enable `AY_ENABLE_VIDEO_DECODE` (defaultOFF), link `AYVideoDecode`, and include
`AYVideo/DecodeSession.h`. The target reuses FFmpegDemuxer/FFmpegDecoder, AYTime and
FFmpeg only. A full AYVideo build can share this target without duplicate backend
compilation. Either find_package(FFMPEG), or supply an existing dependency target
through `AY_VIDEO_DECODE_FFMPEG_TARGET`. Package matching runtime libraries;
`AY_VIDEO_DECODE_RUNTIME_FILES` optionally stages them for the standalone test.

One owner thread drives open/next/seek/info/clearAbort/close. Only requestAbort
is cross-thread; it cooperatively interrupts demux waits. Do not close or seek
concurrently with next. open resets abort and requires a video stream; audio is
optional. next returns owned BGRA pixels and interleaved float PCM, source PTS,
dimensions/channels/rate, or an explicit status. Empty Ok samples are possible;
continue within an outer cancellation/time budget. EndOfStream is explicit.

The host owns real-time pacing, window presentation and PCM output. Owned sample
buffers remain valid after later calls. seek clears cancellation and resets decode
state; callers discard stale presentation queues. Video is bounded to4096x2160;
supported CPU formats BGRA/RGBA/I420/NV12. Other formats fail rather than silently
showing invented frames. No content understanding, subtitles, search or model
integration is included. Codec/URL availability depends on the supplied FFmpeg
build and transport configuration.

```cpp
ayt::video::DecodeSession decode;
ayt::video::DemuxerOpenParams source;
source.path = "clip.mp4";
source.openTimeoutMs = 5000;
source.rwTimeoutMs = 5000;
if (decode.open(source, true) == ayt::video::VideoResult::Ok) {
    ayt::video::DecodedSample sample;
    // Drive next on an owned worker, present sample.bgra and sample.audio.
    // EndOfStream/Cancelled/error must redirect or terminate the host loop.
}
```

Evidence: `decode/Test_DecodeSession.cpp`, generated MPEG4/AAC video/audio, owned
frame reuse, seek, abort/close and actual loopback HTTP demux. No live public URL,
hardware decoder or full-renderer matrix acceptance is implied.

## Optional source policy

DemuxerOpenParams.protocolAllowlist and formatAllowlist accept positive lowercase
comma-separated backend names (up to512bytes); empty keeps existing defaults.
Invalid list syntax rejects before opening. The backend passes these to FFmpeg
protocol_whitelist/format_whitelist, including nested opens. A restricted local
consumer can choose protocol=file and ordinary container demuxers; a network
consumer can choose HTTP(S)/transport protocols and omit file/pipe/concat. The
host still canonicalizes authorized paths and chooses policy; these are not an
OS sandbox. Tests actually reject a file source under an HTTP-only list and an
MP4 under an ASF-only format list, then verify valid file+MOV decode.

httpProxyOrigin optionally supplies an HTTP CONNECT proxy for network sources;
the host resolves any Windows/user proxy policy before opening. The value must be
a credential-free http origin. Empty preserves backend defaults. Video does not
depend on AYNetworkHttp or change OS proxy settings. Transport TLS behavior is
that of the supplied FFmpeg build; this option does not claim new certificate
verification semantics.
