# Standalone full player

`AYVideoPlayback` contains the existing AYVideoPlayer implementation, decode loop,
queues, synchronization clock, track/subtitle handling, color conversion and frame
texture factories. It requires AYTime and AYAudioPlayback; it does not require
GameLoop, ECS or a renderer. AYVideo adds subsystem/runtime-module wrappers and
continues to expose the same player. AYVideoDecode remains the lower-level option
for hosts that deliberately want decoding without playback or synchronization.

Add `AYAudio/playback` and then `AYVideo/playback` after Foundation and AYCore.
If AYVideoDecode already exists its FFmpeg/types are reused; otherwise the usual
FFmpeg package discovery applies, with Null/Mock fallback. A preexisting renderer
target enables the GPU frame texture; the CPU texture works independently.

Initialize a host-owned AudioEngine on the owner thread, attach it before open,
and periodically pull frames. Attach only a functioning device if audible playback
is required. The engine owns video timing and audio-master synchronization; the
host owns the native window's composition and retains the last presented picture.
Do not create a second wall clock or PCM device queue in the window layer.

`openWithOptions(DemuxerOpenParams)` preserves explicit host policy and does not
add default reconnects or broaden protocols. It retains these options through
seek/reconnect. `open(path)` retains its previous HTTP/RTSP/HLS defaults.
The host authorizes local paths before either call. Usual single-owner and frame
lifetime rules apply; stop/join decode before audio or the output surface dies.

`AYVideoPlayback_Tests` reuses all core module tests, excluding subsystem/runtime
and renderer assembly tests. With FFmpeg it includes actual generated media,
playback, decoder-thread stress, audio bridge and option-preservation tests.
