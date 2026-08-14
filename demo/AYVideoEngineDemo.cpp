// AYVideoEngineDemo.cpp 鈥?interactive player + V3 GPU acceptance.
//
// Interactive (default): URL/path bar + Open / Play / Pause / Stop + scrubber.
//   Rejects empty / unsupported schemes; open() failures stay Idle/Stopped.
//   Prefers local AliyatRenderer assets; also accepts http(s) progressive.
//
// CI acceptance (AYVIDEO_DEMO_FRAMES=n): solid RGBA / I420 grey paths
//   (AYVIDEO_DEMO_PATH=1|2), screenshots at frames 30/60 鈥?unchanged.

#ifndef UNICODE
#  define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>
#include <CommCtrl.h>
#include <windowsx.h>

#include "AYGameLoop.h"
#include "AYRendererSubSystem.h"
#include "AYTilemapShaderSources.h"

#include "AYVideoFrame.h"
#include "AYVideoMediaInfo.h"
#include "AYVideoPlayer.h"
#include "AYVideoSyncClock.h"
#include "AYVideoTypes.h"
#include "IAYVideoBackendFactory.h"
#include "IAYVideoDemuxer.h"
#include "IVideoFrameTexture.h"
#include "backend/RendererVideoFrameTexture.h"

#include <AYAudioEngine.h>
#include <AYAudioTypes.h>

#include <aymath/MathTransform.h>
#include <aymath/MathTypes.h>
#include <aymath/MathUtils.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

#pragma comment(lib, "comctl32.lib")

namespace
{

constexpr int kWindowWidth  = 1280;
constexpr int kWindowHeight = 720;
constexpr int kUiBand       = 96;
constexpr int kShotFrames[] = {30, 60};
constexpr int kShotCount    = 2;

constexpr int IDC_URL    = 1001;
constexpr int IDC_OPEN   = 1002;
constexpr int IDC_PLAY   = 1003;
constexpr int IDC_PAUSE  = 1004;
constexpr int IDC_STOP   = 1005;
constexpr int IDC_SCRUB  = 1006;
constexpr int IDC_STATUS = 1007;
constexpr int IDC_TIME   = 1008;

constexpr wchar_t kDefaultLocalW[] =
    L"D:\\Projects\\AliyatRenderer\\assets\\core\\videos\\test_video.mp4";
constexpr wchar_t kDefaultHttpW[] =
    L"http://127.0.0.1:8080/test_video.mp4";

enum class DemoPath : int
{
    SolidRgba = 1,
    I420Grey  = 2,
};

struct DemoState
{
    ayt::game::GameLoop* loop = nullptr;
    HWND hwnd = nullptr;
    HWND renderHost = nullptr; // bgfx swapchain lives here (not on hwnd)
    HWND urlEdit = nullptr;
    HWND statusLabel = nullptr;
    HWND timeLabel = nullptr;
    HWND scrub = nullptr;
    HWND btnOpen = nullptr;
    HWND btnPlay = nullptr;
    HWND btnPause = nullptr;
    HWND btnStop = nullptr;

    bool running = true;
    bool interactive = true;
    bool scrubDragging = false;
    bool scrubWasPlaying = false;
    bool sourceSeekable = true;
    int  scrubPreviewPos = -1;
    ULONGLONG scrubPreviewTick = 0;
    std::int64_t scrubUploadedPtsUs = std::numeric_limits<std::int64_t>::min();
    int  frame = 0;
    int  frameCap = 0;
    int  renderW = kWindowWidth;
    int  renderH = kWindowHeight - kUiBand;
    DemoPath path = DemoPath::SolidRgba;
    std::string screenshotDir;
    std::string statusText = "Ready";

    ayt::render::MeshHandle     quad{};
    ayt::render::MaterialHandle material{};
    std::unique_ptr<ayt::video::IVideoFrameTexture> frameTex;
    ayt::render::DrawPayload2D  payload{};
    bool gpuReady = false;

    std::unique_ptr<ayt::video::AYVideoPlayer> player;
    std::unique_ptr<ayt::audio::AudioEngine> audio;
    ayt::video::MediaInfo media{};
    std::vector<uint8_t> solidStorage; // CI path only
    bool scrubSubclassed = false;
    bool audioAttached = false;
};

void onPause(DemoState& state);

// Click-on-channel → absolute position (default Trackbar pages by a fixed
// step instead of jumping to the pointer).
LRESULT CALLBACK scrubSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam, UINT_PTR /*id*/,
                                   DWORD_PTR ref)
{
    auto* state = reinterpret_cast<DemoState*>(ref);
    if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) && state)
    {
        // Freeze clock→scrub sync for the whole press. Otherwise the game
        // loop's updateTimeLabel() overwrites TBM_SETPOS back to
        // player.position() (often ~0) before ENDTRACK reads the thumb.
        // Capture was-playing HERE: setting scrubDragging early would make
        // TB_THUMBTRACK skip its begin-scrub block, so ENDTRACK never
        // resumed (resume=0) after Keyframe seek pauses Playing.
        if (!state->scrubDragging)
        {
            state->scrubWasPlaying =
                state->player
                && state->player->state()
                       == ayt::video::PlayerState::Playing;
            state->scrubPreviewPos = -1;
            state->scrubPreviewTick = 0;
            if (state->scrubWasPlaying)
            {
                onPause(*state);
            }
        }
        state->scrubDragging = true;

        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT thumb{};
        SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        if (!PtInRect(&thumb, pt))
        {
            RECT channel{};
            SendMessageW(hwnd, TBM_GETCHANNELRECT, 0,
                         reinterpret_cast<LPARAM>(&channel));
            const int minPos =
                static_cast<int>(SendMessageW(hwnd, TBM_GETRANGEMIN, 0, 0));
            const int maxPos =
                static_cast<int>(SendMessageW(hwnd, TBM_GETRANGEMAX, 0, 0));
            int newPos = minPos;
            if (channel.right > channel.left)
            {
                newPos = minPos
                         + MulDiv(pt.x - channel.left, maxPos - minPos,
                                  channel.right - channel.left);
            }
            if (newPos < minPos)
            {
                newPos = minPos;
            }
            if (newPos > maxPos)
            {
                newPos = maxPos;
            }
            SendMessageW(hwnd, TBM_SETPOS, TRUE, newPos);
            std::fprintf(stderr,
                         "[AYVideoDemo][scrub] channel-click pos=%d/1000 "
                         "(freeze scrub sync)\n",
                         newPos);
            std::fflush(stderr);

            // Redirect the click onto the thumb so DefSubclassProc starts a
            // normal thumb-drag from the new position (gives TB_ENDTRACK).
            SendMessageW(hwnd, TBM_GETTHUMBRECT, 0,
                         reinterpret_cast<LPARAM>(&thumb));
            lParam = MAKELPARAM((thumb.left + thumb.right) / 2,
                                (thumb.top + thumb.bottom) / 2);
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

bool fileExists(const std::string& path)
{
    struct stat st {};
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

bool ensureDir(const std::string& path)
{
#ifdef _WIN32
    if (CreateDirectoryA(path.c_str(), nullptr) != 0)
    {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    (void)path;
    return true;
#endif
}

std::string narrow(const std::wstring& w)
{
    if (w.empty())
    {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

std::wstring trim(std::wstring s)
{
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
    {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t'))
    {
        s.pop_back();
    }
    return s;
}

bool startsWithI(const std::wstring& s, const wchar_t* prefix)
{
    const size_t n = wcslen(prefix);
    if (s.size() < n)
    {
        return false;
    }
    for (size_t i = 0; i < n; ++i)
    {
        const wchar_t a = towlower(s[i]);
        const wchar_t b = towlower(prefix[i]);
        if (a != b)
        {
            return false;
        }
    }
    return true;
}

// Returns empty on success; otherwise a human-readable rejection reason.
std::wstring validateSource(const std::wstring& raw, std::string& outPath,
                            bool& outHttp)
{
    outHttp = false;
    outPath.clear();
    const std::wstring src = trim(raw);
    if (src.empty())
    {
        return L"URL/path is empty";
    }
    if (startsWithI(src, L"http://") || startsWithI(src, L"https://"))
    {
        outHttp = true;
        outPath = narrow(src);
        return {};
    }
    if (startsWithI(src, L"ftp://") || startsWithI(src, L"rtsp://")
        || startsWithI(src, L"rtmp://"))
    {
        return L"Unsupported scheme (use http(s) or a local file path)";
    }
    if (startsWithI(src, L"file:"))
    {
        std::wstring path = src.substr(5);
        while (!path.empty() && (path.front() == L'/' || path.front() == L'\\'))
        {
            path.erase(path.begin());
        }
        // file:///D:/x -> D:/x
        outPath = narrow(path);
        if (!fileExists(outPath))
        {
            return L"Local file not found";
        }
        return {};
    }
    // Local path (absolute or relative).
    outPath = narrow(src);
    if (!fileExists(outPath))
    {
        return L"Local file not found (or unsupported URL)";
    }
    return {};
}

std::wstring formatTime(double sec)
{
    if (sec < 0.0)
    {
        sec = 0.0;
    }
    const int total = static_cast<int>(sec);
    const int m = total / 60;
    const int s = total % 60;
    wchar_t buf[32];
    swprintf_s(buf, L"%d:%02d", m, s);
    return buf;
}

void setStatus(DemoState& state, const std::wstring& text)
{
    state.statusText = narrow(text);
    if (state.statusLabel)
    {
        SetWindowTextW(state.statusLabel, text.c_str());
    }
}

void updateScrubPreview(DemoState& state)
{
    if (!state.timeLabel || !state.scrub)
    {
        return;
    }
    const double dur = state.media.durationSec > 0.0 ? state.media.durationSec
                                                     : 0.0;
    const int v = static_cast<int>(SendMessageW(state.scrub, TBM_GETPOS, 0, 0));
    const double pos = dur > 0.0 ? (static_cast<double>(v) / 1000.0) * dur : 0.0;
    SetWindowTextW(state.timeLabel,
                   (formatTime(pos) + L" / " + formatTime(dur) + L"  [seek]")
                       .c_str());
}

void updateTimeLabel(DemoState& state)
{
    if (!state.timeLabel || !state.player)
    {
        return;
    }
    if (state.scrubDragging)
    {
        updateScrubPreview(state);
        return;
    }
    const auto st = state.player->state();
    if (st == ayt::video::PlayerState::Stopped
        || st == ayt::video::PlayerState::Idle
        || st == ayt::video::PlayerState::Failed)
    {
        SetWindowTextW(state.timeLabel, L"0:00 / 0:00");
        if (state.scrub && !state.scrubDragging)
        {
            SendMessageW(state.scrub, TBM_SETPOS, TRUE, 0);
        }
        return;
    }

    double pos = static_cast<double>(state.player->position().toUs())
                 / 1'000'000.0;
    const double dur = state.media.durationSec > 0.0 ? state.media.durationSec
                                                     : 0.0;
    if (dur > 0.0 && pos > dur)
    {
        pos = dur;
    }
    std::wstring t = formatTime(pos) + L" / " + formatTime(dur);
    if (state.player->isBuffering())
    {
        t += L"  [buffering]";
    }
    SetWindowTextW(state.timeLabel, t.c_str());

    if (state.scrub && !state.scrubDragging && dur > 0.0)
    {
        const int v = static_cast<int>(
            std::clamp(pos / dur, 0.0, 1.0) * 1000.0);
        SendMessageW(state.scrub, TBM_SETPOS, TRUE, v);
    }
}

void layoutUi(DemoState& state)
{
    if (!state.hwnd || !state.interactive)
    {
        return;
    }
    RECT rc{};
    GetClientRect(state.hwnd, &rc);
    const int w = std::max(1, static_cast<int>(rc.right - rc.left));
    const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
    const int pad = 8;
    const int row1Y = 8;
    const int row1H = 24;
    const int btnW = 64;
    const int openW = 72;
    int x = pad;
    if (state.urlEdit)
    {
        MoveWindow(state.urlEdit, x, row1Y, w - openW - pad * 3, row1H, TRUE);
        x = w - openW - pad;
    }
    if (state.btnOpen)
    {
        MoveWindow(state.btnOpen, x, row1Y, openW, row1H, TRUE);
    }

    const int row2Y = row1Y + row1H + 8;
    x = pad;
    auto placeBtn = [&](HWND hwndBtn) {
        if (hwndBtn)
        {
            MoveWindow(hwndBtn, x, row2Y, btnW, row1H, TRUE);
            x += btnW + 6;
        }
    };
    placeBtn(state.btnPlay);
    placeBtn(state.btnPause);
    placeBtn(state.btnStop);
    if (state.scrub)
    {
        const int scrubW = std::max(120, w - x - 140 - pad);
        MoveWindow(state.scrub, x, row2Y + 2, scrubW, row1H, TRUE);
        x += scrubW + 8;
    }
    if (state.timeLabel)
    {
        MoveWindow(state.timeLabel, x, row2Y, w - x - pad, row1H, TRUE);
    }
    if (state.statusLabel)
    {
        MoveWindow(state.statusLabel, pad, row2Y + row1H + 8, w - pad * 2, 20,
                   TRUE);
    }

    // GPU view occupies everything below the UI band 鈥?separate HWND so
    // bgfx cannot paint over EDIT/BUTTON siblings.
    state.renderW = w;
    state.renderH = std::max(1, h - kUiBand);
    if (state.renderHost)
    {
        MoveWindow(state.renderHost, 0, kUiBand, state.renderW, state.renderH,
                   TRUE);
        // Keep UI above the render surface in z-order.
        SetWindowPos(state.renderHost, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (ayt::render::RendererSubSystem* rss =
            ayt::render::RendererSubSystem::findRegistered())
    {
        rss->setClientSize(static_cast<uint32_t>(state.renderW),
                           static_cast<uint32_t>(state.renderH));
    }
}

void createUi(DemoState& state)
{
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    const HINSTANCE inst = GetModuleHandleW(nullptr);

    // Dedicated child HWND for bgfx 鈥?must not share the shell HWND with
    // Win32 controls (swapchain covers the whole client area otherwise).
    WNDCLASSEXW rc{};
    rc.cbSize = sizeof(rc);
    rc.style = CS_OWNDC;
    rc.lpfnWndProc = DefWindowProcW;
    rc.hInstance = inst;
    rc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    rc.hbrBackground = nullptr;
    rc.lpszClassName = L"AYVideoRenderHost";
    RegisterClassExW(&rc);

    state.renderHost = CreateWindowExW(
        0, L"AYVideoRenderHost", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, kUiBand, state.renderW,
        state.renderH, state.hwnd, nullptr, inst, nullptr);

    const DWORD editStyle =
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_LEFT
        | WS_CLIPSIBLINGS;
    const DWORD btnStyle =
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_CLIPSIBLINGS;
    const DWORD staticStyle = WS_CHILD | WS_VISIBLE | SS_LEFT | WS_CLIPSIBLINGS;

    state.urlEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", editStyle,
                                    0, 0, 100, 24, state.hwnd,
                                    reinterpret_cast<HMENU>(
                                        static_cast<INT_PTR>(IDC_URL)),
                                    inst, nullptr);
    state.btnOpen = CreateWindowW(L"BUTTON", L"Open", btnStyle, 0, 0, 72, 24,
                                  state.hwnd,
                                  reinterpret_cast<HMENU>(
                                      static_cast<INT_PTR>(IDC_OPEN)),
                                  inst, nullptr);
    state.btnPlay = CreateWindowW(L"BUTTON", L"Play", btnStyle, 0, 0, 64, 24,
                                  state.hwnd,
                                  reinterpret_cast<HMENU>(
                                      static_cast<INT_PTR>(IDC_PLAY)),
                                  inst, nullptr);
    state.btnPause = CreateWindowW(L"BUTTON", L"Pause", btnStyle, 0, 0, 64, 24,
                                   state.hwnd,
                                   reinterpret_cast<HMENU>(
                                       static_cast<INT_PTR>(IDC_PAUSE)),
                                   inst, nullptr);
    state.btnStop = CreateWindowW(L"BUTTON", L"Stop", btnStyle, 0, 0, 64, 24,
                                  state.hwnd,
                                  reinterpret_cast<HMENU>(
                                      static_cast<INT_PTR>(IDC_STOP)),
                                  inst, nullptr);
    state.scrub = CreateWindowW(TRACKBAR_CLASSW, L"",
                                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS
                                    | WS_CLIPSIBLINGS,
                                0, 0, 200, 24, state.hwnd,
                                reinterpret_cast<HMENU>(
                                    static_cast<INT_PTR>(IDC_SCRUB)),
                                inst, nullptr);
    SendMessageW(state.scrub, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
    SendMessageW(state.scrub, TBM_SETPOS, TRUE, 0);
    // Page step unused for channel-click (subclassed to absolute jump);
    // keep a modest line step for arrow keys.
    SendMessageW(state.scrub, TBM_SETPAGESIZE, 0, 1);
    SendMessageW(state.scrub, TBM_SETLINESIZE, 0, 10);
    if (SetWindowSubclass(state.scrub, scrubSubclassProc, 1,
                          reinterpret_cast<DWORD_PTR>(&state)))
    {
        state.scrubSubclassed = true;
    }

    state.timeLabel = CreateWindowW(L"STATIC", L"0:00 / 0:00", staticStyle, 0,
                                    0, 120, 24, state.hwnd,
                                    reinterpret_cast<HMENU>(
                                        static_cast<INT_PTR>(IDC_TIME)),
                                    inst, nullptr);
    state.statusLabel = CreateWindowW(L"STATIC", L"Ready", staticStyle, 0, 0,
                                      400, 20, state.hwnd,
                                      reinterpret_cast<HMENU>(
                                          static_cast<INT_PTR>(IDC_STATUS)),
                                      inst, nullptr);

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    for (HWND h : {state.urlEdit, state.btnOpen, state.btnPlay, state.btnPause,
                    state.btnStop, state.timeLabel, state.statusLabel})
    {
        if (h)
        {
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
    }

    std::wstring def = kDefaultLocalW;
    if (!fileExists(narrow(def)))
    {
        def = kDefaultHttpW;
    }
    SetWindowTextW(state.urlEdit, def.c_str());
    layoutUi(state);
}

bool ensurePlayer(DemoState& state)
{
    if (state.player)
    {
        return true;
    }
    auto demux = ayt::video::makeFFmpegDemuxer();
    auto decoder = ayt::video::makeFFmpegDecoder();
    if (!demux || !decoder)
    {
        setStatus(state, L"FFmpeg backends unavailable");
        MessageBoxW(state.hwnd, L"FFmpeg backends unavailable in this build.",
                    L"AYVideo Demo", MB_ICONERROR | MB_OK);
        return false;
    }
    state.player = std::make_unique<ayt::video::AYVideoPlayer>(
        std::move(demux), std::move(decoder), nullptr);
    state.player->setBufferWatermarks(0, 2);
    state.player->setOnBufferingChanged([&state](bool buffering) {
        if (buffering)
        {
            setStatus(state, L"Buffering...");
        }
        else if (state.player
                 && state.player->state() == ayt::video::PlayerState::Playing)
        {
            setStatus(state, L"Playing");
        }
    });

    // Attach AYAudio before open (Idle only). Miniaudio for audible A/V
    // verification; AYVIDEO_DEMO_AUDIO=0 forces Null (silent bridge still
    // exercises decodeAudio / AudioMaster wiring).
    if (!state.audio)
    {
        state.audio = std::make_unique<ayt::audio::AudioEngine>();
        ayt::audio::AudioSettings settings{};
        settings.sampleRate = 48000;
        settings.channels = 2;
        settings.commandQueueCapacity = 64;
        settings.maxVoices = 8;
        settings.maxClips = 8;
        char envAudio[8] = {};
        const bool wantNull =
            GetEnvironmentVariableA("AYVIDEO_DEMO_AUDIO", envAudio,
                                    static_cast<DWORD>(sizeof(envAudio)))
                > 0
            && (envAudio[0] == '0');
        settings.backend = wantNull ? ayt::audio::AudioBackendKind::Null
                                    : ayt::audio::AudioBackendKind::Miniaudio;
        if (!state.audio->initialize(settings))
        {
            std::fprintf(stderr,
                         "[AYVideoDemo] AudioEngine initialize failed "
                         "(backend=%s) — continuing video-only\n",
                         wantNull ? "Null" : "Miniaudio");
            state.audio.reset();
        }
        else
        {
            std::fprintf(stderr,
                         "[AYVideoDemo] AudioEngine ready backend=%s\n",
                         wantNull ? "Null" : "Miniaudio");
        }
    }
    if (state.audio && !state.audioAttached)
    {
        const auto ar = state.player->attachAudioEngine(state.audio.get());
        state.audioAttached = (ar == ayt::video::VideoResult::Ok);
        if (!state.audioAttached)
        {
            std::fprintf(stderr,
                         "[AYVideoDemo] attachAudioEngine failed: %s\n",
                         ayt::video::toString(ar));
        }
    }
    return true;
}

void onOpen(DemoState& state)
{
    if (!ensurePlayer(state))
    {
        return;
    }
    wchar_t buf[2048] = {};
    GetWindowTextW(state.urlEdit, buf, 2048);
    std::string path;
    bool http = false;
    const std::wstring err = validateSource(buf, path, http);
    if (!err.empty())
    {
        setStatus(state, L"Rejected: " + err);
        MessageBoxW(state.hwnd, err.c_str(), L"Invalid source",
                    MB_ICONWARNING | MB_OK);
        return;
    }

    if (state.player->state() != ayt::video::PlayerState::Idle
        && state.player->state() != ayt::video::PlayerState::Stopped)
    {
        (void)state.player->stop();
    }

    const ayt::video::VideoResult r = state.player->open(path);
    if (r != ayt::video::VideoResult::Ok)
    {
        const std::wstring msg =
            L"Open failed: " + widen(ayt::video::toString(r));
        setStatus(state, msg);
        MessageBoxW(state.hwnd, msg.c_str(), L"Open failed",
                    MB_ICONERROR | MB_OK);
        return;
    }

    state.media = {};
    (void)state.player->getMediaInfo(state.media);
    // HTTP progressive is seekable when the server supports Range (V5).
    state.sourceSeekable = true;
    EnableWindow(state.scrub, TRUE);
    setStatus(state,
              L"Opened "
                  + widen(std::to_string(state.media.width) + "x"
                          + std::to_string(state.media.height) + " "
                          + state.media.videoCodec)
                  + (state.media.hasAudio
                         ? L" +audio"
                         : L" (video-only)")
                  + (http ? L" (HTTP)" : L"")
                  + (state.audioAttached ? L" [AYAudio]" : L""));
    updateTimeLabel(state);
}

void onPlay(DemoState& state)
{
    if (!state.player)
    {
        setStatus(state, L"Open a source first");
        return;
    }
    const auto st = state.player->state();
    if (st != ayt::video::PlayerState::Ready
        && st != ayt::video::PlayerState::Paused)
    {
        setStatus(state, L"Nothing to play (open a source first)");
        return;
    }
    const ayt::video::VideoResult r = state.player->play();
    if (r != ayt::video::VideoResult::Ok)
    {
        setStatus(state, L"Play failed: " + widen(ayt::video::toString(r)));
        return;
    }
    setStatus(state,
              L"Playing sync="
                  + widen(ayt::video::toString(state.player->syncSource())));
}

void onPause(DemoState& state)
{
    if (!state.player)
    {
        return;
    }
    if (state.player->pause() == ayt::video::VideoResult::Ok)
    {
        setStatus(state, L"Paused");
    }
}

void onStop(DemoState& state)
{
    if (!state.player)
    {
        return;
    }
    (void)state.player->stop();
    state.media = {};
    if (state.scrub)
    {
        SendMessageW(state.scrub, TBM_SETPOS, TRUE, 0);
    }
    setStatus(state, L"Stopped");
    updateTimeLabel(state);
}

void uploadFrame(DemoState& state, ayt::render::Renderer& renderer,
                 const ayt::video::VideoFrame& frame);

void onScrubSeek(DemoState& state, ayt::video::AYVideoPlayer::SeekMode mode,
                 int scrubPos1000 = -1)
{
    if (!state.player || !state.sourceSeekable || !state.scrub)
    {
        return;
    }
    if (state.media.durationSec <= 0.0)
    {
        return;
    }
    const int v = (scrubPos1000 >= 0)
                      ? scrubPos1000
                      : static_cast<int>(SendMessageW(state.scrub, TBM_GETPOS, 0, 0));
    double sec = (static_cast<double>(v) / 1000.0) * state.media.durationSec;
    // Leave a tiny epsilon off the true end — seeking to exact EOF on
    // some MP4s triggers mov "partial file" reads.
    if (state.media.durationSec > 0.05 && sec > state.media.durationSec - 0.05)
    {
        sec = state.media.durationSec - 0.05;
    }
    if (sec < 0.0)
    {
        sec = 0.0;
    }
    const auto target =
        ayt::time::Duration::fromUs(static_cast<std::int64_t>(sec * 1'000'000.0));
    std::fprintf(stderr,
                 "[AYVideoDemo][scrub] seek mode=%s scrub=%d/1000 target=%.3fs "
                 "clock=%.3fs\n",
                 mode == ayt::video::AYVideoPlayer::SeekMode::Keyframe
                     ? "Keyframe"
                     : (mode == ayt::video::AYVideoPlayer::SeekMode::Scrub
                            ? "Scrub"
                            : "Accurate"),
                 v, sec,
                 state.player->position().toUs() / 1e6);
    std::fflush(stderr);
    const ayt::video::VideoResult r = state.player->seek(target, mode);
    if (r != ayt::video::VideoResult::Ok)
    {
        if (mode == ayt::video::AYVideoPlayer::SeekMode::Accurate)
        {
            setStatus(state, L"Seek failed: " + widen(ayt::video::toString(r)));
        }
        return;
    }
    if (mode == ayt::video::AYVideoPlayer::SeekMode::Accurate)
    {
        setStatus(state, L"Seek OK");
        // Accurate may finish frame harvest on the decode thread — pick it up.
        state.player->pollSeekPreview();
    }
    updateTimeLabel(state);

    // Scrub/Keyframe finish on the decode thread — tickPlayback harvests.
    if (mode == ayt::video::AYVideoPlayer::SeekMode::Keyframe
        || mode == ayt::video::AYVideoPlayer::SeekMode::Scrub)
    {
        return;
    }
    if (state.gpuReady)
    {
        ayt::render::RendererSubSystem* rss =
            ayt::render::RendererSubSystem::findRegistered();
        if (rss)
        {
            ayt::video::VideoFrame f{};
            if (state.player->currentFrame(f) == ayt::video::VideoResult::Ok
                && f.data != nullptr)
            {
                uploadFrame(state, rss->renderer(), f);
                state.scrubUploadedPtsUs = f.pts.toUs();
            }
        }
    }
}

void onScrubPreview(DemoState& state)
{
    // WMP-like phase: coalesced Scrub seeks while dragging (decode walk
    // with live ceiling; tick paints latest frame).
    if (!state.player || !state.sourceSeekable || !state.scrub)
    {
        return;
    }
    const int v = static_cast<int>(SendMessageW(state.scrub, TBM_GETPOS, 0, 0));
    const ULONGLONG now = GetTickCount64();
    constexpr ULONGLONG kMinIntervalMs = 33;
    constexpr int kMinPosDelta = 4;
    updateScrubPreview(state);
    if (state.scrubPreviewPos >= 0)
    {
        const int dPos = std::abs(v - state.scrubPreviewPos);
        const ULONGLONG dt = now - state.scrubPreviewTick;
        if (dPos == 0)
        {
            return;
        }
        if (dPos < kMinPosDelta && dt < kMinIntervalMs)
        {
            return;
        }
        if (dt < kMinIntervalMs && dPos < 10)
        {
            return;
        }
    }
    state.scrubPreviewPos = v;
    state.scrubPreviewTick = now;
    onScrubSeek(state, ayt::video::AYVideoPlayer::SeekMode::Scrub);
}

ayt::video::VideoFrame makeSolidRgba(std::vector<uint8_t>& storage,
                                     int32_t w, int32_t h,
                                     uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    storage.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (size_t i = 0; i < storage.size(); i += 4)
    {
        storage[i + 0] = r;
        storage[i + 1] = g;
        storage[i + 2] = b;
        storage[i + 3] = a;
    }
    ayt::video::VideoFrame f{};
    f.data = storage.data();
    f.dataSize = static_cast<uint32_t>(storage.size());
    f.width = w;
    f.height = h;
    f.stride = static_cast<uint32_t>(w) * 4u;
    f.format = ayt::video::VideoPixelFormat::RGBA8;
    return f;
}

ayt::video::VideoFrame makeI420Grey(std::vector<uint8_t>& storage,
                                    int32_t w, int32_t h, uint8_t yValue)
{
    const uint32_t cw = static_cast<uint32_t>((w + 1) / 2);
    const uint32_t ch = static_cast<uint32_t>((h + 1) / 2);
    const size_t yBytes = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t uBytes = static_cast<size_t>(cw) * ch;
    storage.resize(yBytes + uBytes + uBytes);
    std::memset(storage.data(), yValue, yBytes);
    std::memset(storage.data() + yBytes, 128, uBytes + uBytes);

    ayt::video::VideoFrame f{};
    f.data = storage.data();
    f.dataSize = static_cast<uint32_t>(storage.size());
    f.width = w;
    f.height = h;
    f.stride = static_cast<uint32_t>(w);
    f.format = ayt::video::VideoPixelFormat::I420;
    f.planeOffset[0] = 0;
    f.planeOffset[1] = static_cast<uint32_t>(yBytes);
    f.planeOffset[2] = static_cast<uint32_t>(yBytes + uBytes);
    return f;
}

void bindTexture(DemoState& state, ayt::render::Renderer& renderer)
{
    auto* gpu = dynamic_cast<ayt::video::RendererVideoFrameTexture*>(
        state.frameTex.get());
    if (gpu == nullptr || !gpu->gpuHandle().isValid() || !state.material.isValid())
    {
        return;
    }
    renderer.setMaterialTexture(state.material, "albedoMap", gpu->gpuHandle());
    state.frameTex->clearDirty();
}

void uploadFrame(DemoState& state, ayt::render::Renderer& renderer,
                 const ayt::video::VideoFrame& frame)
{
    if (!state.frameTex)
    {
        state.frameTex = ayt::video::makeRendererVideoFrameTexture(renderer);
        if (!state.frameTex)
        {
            return;
        }
    }
    if (frame.data == nullptr || frame.dataSize == 0)
    {
        return;
    }
    if (state.frameTex->updateFromFrame(frame) != ayt::video::VideoResult::Ok)
    {
        return;
    }
    bindTexture(state, renderer);
}

void uploadAcceptanceFrame(DemoState& state, ayt::render::Renderer& renderer)
{
    ayt::video::VideoFrame frame{};
    if (state.path == DemoPath::I420Grey)
    {
        frame = makeI420Grey(state.solidStorage, 64, 36, 128);
    }
    else
    {
        frame = makeSolidRgba(state.solidStorage, 64, 36, 255, 0, 255, 255);
    }
    uploadFrame(state, renderer, frame);
}

void ensureGpu(DemoState& state)
{
    if (state.gpuReady)
    {
        return;
    }
    ayt::render::RendererSubSystem* rss =
        ayt::render::RendererSubSystem::findRegistered();
    if (rss == nullptr)
    {
        return;
    }
    ayt::render::Renderer& renderer = rss->renderer();
    state.quad = renderer.createUnitQuad();
    state.material = renderer.createMaterialFromPhoskia(
        ayt::render::kTilemapPhoskiaSource, "ayvideo_engine_demo");
    if (!state.material.isValid() || !state.quad.isValid())
    {
        std::fprintf(stderr, "[AYVideoDemo] material/quad acquire failed "
                     "(shaderc hint exists=%d)\n",
                     fileExists(AY_SHADER_SHADERC_HINT) ? 1 : 0);
        return;
    }
    if (!state.interactive)
    {
        uploadAcceptanceFrame(state, renderer);
    }
    else
    {
        // Placeholder dark frame until Open+Play.
        uploadFrame(state, renderer,
                    makeSolidRgba(state.solidStorage, 64, 36, 24, 24, 28, 255));
    }
    state.payload.sourceRectMin = ayt::math::FVector2(0.0f, 0.0f);
    state.payload.sourceRectMax = ayt::math::FVector2(1.0f, 1.0f);
    state.payload.tintRGBA = ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f);
    state.gpuReady = state.frameTex && state.frameTex->gpuTextureId() != 0;
    std::fprintf(stderr, "[AYVideoDemo] GPU ready=%d texId=%llu interactive=%d\n",
                 state.gpuReady ? 1 : 0,
                 static_cast<unsigned long long>(
                     state.frameTex ? state.frameTex->gpuTextureId() : 0),
                 state.interactive ? 1 : 0);
    std::fprintf(stderr,
                 "[AYVideoDemo] seek timing: set AYVIDEO_SEEK_LOG=1 for full "
                 "traces (slow stages >=8ms always print)\n");
    std::fflush(stderr);
}

void tickPlayback(DemoState& state)
{
    if (!state.interactive || !state.player || !state.gpuReady)
    {
        return;
    }
    // Drive AYAudio command pump every frame (required for stream voices).
    if (state.audio && state.audio->isInitialized())
    {
        state.audio->submitFrame(0.0f);
    }
    ayt::render::RendererSubSystem* rss =
        ayt::render::RendererSubSystem::findRegistered();
    if (rss == nullptr)
    {
        return;
    }

    if (state.scrubDragging)
    {
        // In-loop Keyframe seeks finish on the decode thread — harvest
        // here so the UI thread never blocks on demux/decode.
        state.player->pollSeekPreview();
        ayt::video::VideoFrame f{};
        if (state.player->currentFrame(f) == ayt::video::VideoResult::Ok
            && f.data != nullptr)
        {
            const std::int64_t ptsUs = f.pts.toUs();
            if (ptsUs != state.scrubUploadedPtsUs)
            {
                uploadFrame(state, rss->renderer(), f);
                state.scrubUploadedPtsUs = ptsUs;
            }
        }
        updateTimeLabel(state);
        return;
    }

    if (state.player->state() == ayt::video::PlayerState::Playing)
    {
        // Accurate floor may still be pending after scrub-release resume.
        state.player->pollSeekPreview();
        ayt::video::VideoFrame f{};
        const ayt::video::VideoResult r = state.player->pullFrame(f);
        if (r == ayt::video::VideoResult::Ok && f.data != nullptr)
        {
            uploadFrame(state, rss->renderer(), f);
            state.scrubUploadedPtsUs = f.pts.toUs();
        }
        else if (r == ayt::video::VideoResult::Ok)
        {
            if (state.player->currentFrame(f) == ayt::video::VideoResult::Ok
                && f.data != nullptr)
            {
                const std::int64_t ptsUs = f.pts.toUs();
                if (ptsUs != state.scrubUploadedPtsUs)
                {
                    uploadFrame(state, rss->renderer(), f);
                    state.scrubUploadedPtsUs = ptsUs;
                }
            }
        }
        else if (r == ayt::video::VideoResult::EndOfStream)
        {
            setStatus(state, L"End of stream");
        }
        else if (r != ayt::video::VideoResult::Ok)
        {
            setStatus(state,
                      L"Playback error: " + widen(ayt::video::toString(r)));
        }
    }
    else if (state.player->state() == ayt::video::PlayerState::Paused)
    {
        // Accurate floor may finish asynchronously — one snap when ready.
        state.player->pollSeekPreview();
        ayt::video::VideoFrame f{};
        if (state.player->currentFrame(f) == ayt::video::VideoResult::Ok
            && f.data != nullptr)
        {
            const std::int64_t ptsUs = f.pts.toUs();
            if (ptsUs != state.scrubUploadedPtsUs)
            {
                uploadFrame(state, rss->renderer(), f);
                state.scrubUploadedPtsUs = ptsUs;
            }
        }
    }
    updateTimeLabel(state);
}

void buildScene(DemoState& state, ayt::render::RenderScene& scene)
{
    ensureGpu(state);
    ayt::render::RendererSubSystem* rss =
        ayt::render::RendererSubSystem::findRegistered();
    if (rss == nullptr || !state.gpuReady)
    {
        return;
    }
    ayt::render::Renderer& renderer = rss->renderer();

    const float halfW = static_cast<float>(state.renderW) * 0.5f;
    const float halfH = static_cast<float>(state.renderH) * 0.5f;
    renderer.setMainCamera(ayt::math::Float4x4::identity(),
                           ayt::math::ortho(-halfW, halfW, -halfH, halfH,
                                            -1.0f, 1.0f));

    // Fill the render-host HWND (letterboxed slightly for acceptance path).
    const float videoW = state.interactive
                             ? static_cast<float>(state.renderW) * 0.92f
                             : 640.0f;
    const float videoH = state.interactive
                             ? static_cast<float>(state.renderH) * 0.92f
                             : 360.0f;

    ayt::render::DrawItem item;
    item.mesh = state.quad;
    item.material = state.material;
    item.payload = &state.payload;
    item.shadowFlags = ayt::render::ShadowFlags::None;
    item.world = ayt::math::Transform::getMatrix(
        ayt::math::FVector3(0.0f, 0.0f, 0.0f),
        ayt::math::FQuaternion::identity(),
        ayt::math::FVector3(videoW, videoH, 1.0f));
    scene.add(item);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DemoState* state =
        reinterpret_cast<DemoState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_ERASEBKGND:
        if (state && state->interactive)
        {
            // Paint only the UI band; renderHost owns the rest.
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            rc.bottom = kUiBand;
            FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            return 1;
        }
        break;
    case WM_SIZE:
        if (state)
        {
            if (state->interactive)
            {
                layoutUi(*state);
            }
            else
            {
                state->renderW = LOWORD(lParam);
                state->renderH = HIWORD(lParam);
                if (ayt::render::RendererSubSystem* rss =
                        ayt::render::RendererSubSystem::findRegistered())
                {
                    rss->setClientSize(static_cast<uint32_t>(state->renderW),
                                       static_cast<uint32_t>(state->renderH));
                }
            }
        }
        return 0;
    case WM_COMMAND:
        if (state && state->interactive)
        {
            switch (LOWORD(wParam))
            {
            case IDC_OPEN:
                onOpen(*state);
                return 0;
            case IDC_PLAY:
                onPlay(*state);
                return 0;
            case IDC_PAUSE:
                onPause(*state);
                return 0;
            case IDC_STOP:
                onStop(*state);
                return 0;
            default:
                break;
            }
        }
        break;
    case WM_HSCROLL:
        if (state && state->interactive && state->scrub
            && reinterpret_cast<HWND>(lParam) == state->scrub)
        {
            const int code = LOWORD(wParam);
            // Drag: begin-scrub (wasPlaying + pause) is armed in the
            // trackbar subclass on LBUTTONDOWN; here only preview.
            if (code == TB_THUMBTRACK)
            {
                if (!state->scrubDragging)
                {
                    // Fallback if a track notification arrives without a
                    // subclassed button-down (e.g. keyboard).
                    state->scrubDragging = true;
                    state->scrubPreviewPos = -1;
                    state->scrubPreviewTick = 0;
                    state->scrubWasPlaying =
                        state->player
                        && state->player->state()
                               == ayt::video::PlayerState::Playing;
                    if (state->scrubWasPlaying)
                    {
                        onPause(*state);
                    }
                }
                onScrubPreview(*state);
                return 0;
            }
            // Windows Media Player-class scrub:
            //   THUMBTRACK / ENDTRACK → SeekMode::Scrub (live ceiling walk,
            //   latest-frame paint). No Accurate stall on click/release.
            if (code == TB_ENDTRACK || code == TB_LINEUP || code == TB_LINEDOWN
                || code == TB_TOP || code == TB_BOTTOM)
            {
                const int v =
                    static_cast<int>(SendMessageW(state->scrub, TBM_GETPOS, 0, 0));
                const bool resume = state->scrubWasPlaying;
                const bool hadDrag = state->scrubDragging;
                const int previewPos = state->scrubPreviewPos;
                std::fprintf(stderr,
                             "[AYVideoDemo][scrub] ENDTRACK pos=%d preview=%d "
                             "hadDrag=%d resume=%d commit=Scrub\n",
                             v, previewPos, hadDrag ? 1 : 0, resume ? 1 : 0);
                std::fflush(stderr);
                onScrubSeek(*state, ayt::video::AYVideoPlayer::SeekMode::Scrub,
                            v);
                state->scrubDragging = false;
                state->scrubWasPlaying = false;
                state->scrubPreviewPos = -1;
                if (resume
                    && state->player
                    && (state->player->state() == ayt::video::PlayerState::Paused
                        || state->player->state()
                               == ayt::video::PlayerState::Ready))
                {
                    onPlay(*state);
                }
            }
            // Ignore TB_PAGEUP/PAGEDOWN — default click-paging is replaced
            // by the subclass absolute jump.
        }
        return 0;
    case WM_CLOSE:
        if (state != nullptr)
        {
            state->running = false;
            if (state->loop != nullptr)
            {
                state->loop->stop();
            }
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state && state->scrubSubclassed && state->scrub)
        {
            RemoveWindowSubclass(state->scrub, scrubSubclassProc, 1);
            state->scrubSubclassed = false;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND createDemoWindow(DemoState* state)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AYVideoEngineDemo";
    RegisterClassExW(&wc);

    RECT rect{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const wchar_t* title = state->interactive
                               ? L"AYVideo Demo 鈥?URL / playbar"
                               : L"AYVideo Engine Demo 鈥?GPU frame texture";
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, title,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
        instance, nullptr);
    if (hwnd != nullptr)
    {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->hwnd = hwnd;
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

void pumpWin32Messages(DemoState& state)
{
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            state.running = false;
            if (state.loop != nullptr)
            {
                state.loop->stop();
            }
        }
        // Let Edit controls receive typing / shortcuts.
        if (!IsDialogMessageW(state.hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

} // namespace

int main()
{
    DemoState state;
    ayt::game::GameLoop& loop = ayt::game::GameLoop::instance();
    state.loop = &loop;

    char envPath[8] = {};
    if (GetEnvironmentVariableA("AYVIDEO_DEMO_PATH", envPath,
                                static_cast<DWORD>(sizeof(envPath))) > 0
        && std::string(envPath) == "2")
    {
        state.path = DemoPath::I420Grey;
    }

    char envFrames[16] = {};
    if (GetEnvironmentVariableA("AYVIDEO_DEMO_FRAMES", envFrames,
                                static_cast<DWORD>(sizeof(envFrames))) > 0)
    {
        state.frameCap = std::atoi(envFrames);
    }
    // CI / screenshot mode keeps the solid texture path (no player UI).
    state.interactive = (state.frameCap <= 0);

    char tempDir[MAX_PATH] = {};
    std::string root = "ayvideo_engine_demo\\";
    if (GetTempPathA(MAX_PATH, tempDir) > 0)
    {
        root = std::string(tempDir) + "ayvideo_engine_demo\\";
    }
    if (!ensureDir(root))
    {
        std::fprintf(stderr, "[AYVideoDemo] failed to create %s\n", root.c_str());
        return 1;
    }
    state.screenshotDir = root + "shots\\";
    if (!ensureDir(state.screenshotDir))
    {
        std::fprintf(stderr, "[AYVideoDemo] failed to create shots dir\n");
        return 1;
    }

    HWND hwnd = createDemoWindow(&state);
    if (hwnd == nullptr)
    {
        std::fprintf(stderr, "[AYVideoDemo] failed to create window\n");
        return 1;
    }
    if (state.interactive)
    {
        createUi(state);
        layoutUi(state);
        ayt::render::RendererSubSystem::setBootstrapWindow(
            state.renderHost, static_cast<uint32_t>(state.renderW),
            static_cast<uint32_t>(state.renderH));
    }
    else
    {
        state.renderW = kWindowWidth;
        state.renderH = kWindowHeight;
        ayt::render::RendererSubSystem::setBootstrapWindow(
            hwnd, static_cast<uint32_t>(state.renderW),
            static_cast<uint32_t>(state.renderH));
    }

    std::fprintf(stderr, "[AYVideoDemo] shaderc hint: %s (exists=%d)\n",
                 AY_SHADER_SHADERC_HINT,
                 fileExists(AY_SHADER_SHADERC_HINT) ? 1 : 0);
    if (state.interactive)
    {
        std::fprintf(stderr,
                     "[AYVideoDemo] interactive | Esc quit | Open URL/path then "
                     "Play | scrub seek (HTTP needs Range)\n");
    }
    else
    {
        std::fprintf(stderr,
                     "[AYVideoDemo] path=%d | Esc quit | shots at 30/60 | "
                     "AYVIDEO_DEMO_FRAMES=%d | shots=%s\n",
                     static_cast<int>(state.path), state.frameCap,
                     state.screenshotDir.c_str());
    }

    loop.setTargetFPS(60.0f);
    loop.setRenderThreadEnabled(false);
    ayt::render::RendererSubSystem::registerSubSystem();

    ayt::render::RendererSubSystem* rss =
        ayt::render::RendererSubSystem::findRegistered();
    if (rss != nullptr)
    {
        rss->setSceneBuilder([&state](ayt::render::RenderScene& scene) {
            buildScene(state, scene);
        });
    }

    const uint64_t listenerId = loop.onUpdate([&](float /*dt*/) {
        pumpWin32Messages(state);
        if (!state.running)
        {
            loop.stop();
            return;
        }

        tickPlayback(state);

        if (!state.interactive)
        {
            for (int i = 0; i < kShotCount; ++i)
            {
                if (state.frame == kShotFrames[i])
                {
                    char name[64];
                    std::snprintf(name, sizeof(name), "frame_%02d", state.frame);
                    const std::string base = state.screenshotDir + name;
                    ayt::render::RendererSubSystem* shotRss =
                        ayt::render::RendererSubSystem::findRegistered();
                    if (shotRss != nullptr
                        && shotRss->renderer().captureScreenshot(base))
                    {
                        std::fprintf(stderr,
                                     "[AYVideoDemo] frame %d -> %s (.tga/.png)\n",
                                     state.frame, base.c_str());
                    }
                }
            }
            if (state.frameCap > 0 && state.frame >= state.frameCap)
            {
                std::fprintf(stderr, "[AYVideoDemo] reached %d frames; stopping\n",
                             state.frameCap);
                state.running = false;
                loop.stop();
                return;
            }
        }
        state.frame++;

        // Esc quits only when the URL edit does not have focus (so typing
        // Esc in a path does not close the window unexpectedly). Esc on
        // empty focus still works via GetAsyncKeyState when not typing.
        const HWND focus = GetFocus();
        const bool typing = (focus == state.urlEdit);
        if (!typing && (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
        {
            state.running = false;
            loop.stop();
        }
    });

    loop.run();
    loop.offUpdate(listenerId);
    if (state.player)
    {
        (void)state.player->stop();
        (void)state.player->attachAudioEngine(nullptr);
        state.audioAttached = false;
        state.player.reset();
    }
    if (state.audio)
    {
        state.audio->shutdown();
        state.audio.reset();
    }
    loop.shutdown();

    std::fprintf(stderr, "[AYVideoDemo] done. frames=%d gpuReady=%d\n",
                 state.frame, state.gpuReady ? 1 : 0);
    std::printf("AYVideo_EngineDemo finished.\n");
    return state.gpuReady ? 0 : 2;
}
