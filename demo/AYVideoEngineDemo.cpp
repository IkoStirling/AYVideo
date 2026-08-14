// AYVideoEngineDemo.cpp — V3 GPU frame texture on-screen acceptance.
//
// Two upload paths (AYVIDEO_DEMO_PATH=1|2, default 1):
//   1 = solid RGBA8 magenta → makeRendererVideoFrameTexture
//   2 = I420 mid-grey → convert + same GPU texture
//
// Host mirrors AY2D_EngineDemo: Win32 + GameLoop + RendererSubSystem,
// textured unit quad via kTilemapPhoskiaSource / albedoMap.
// Screenshots at frames 30/60; AYVIDEO_DEMO_FRAMES=n auto-exit.

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

#include "AYGameLoop.h"
#include "AYRendererSubSystem.h"
#include "AYTilemapShaderSources.h"

#include "AYVideoFrame.h"
#include "AYVideoTypes.h"
#include "IAYVideoBackendFactory.h"
#include "IVideoFrameTexture.h"
#include "backend/RendererVideoFrameTexture.h"

#include <aymath/MathTransform.h>
#include <aymath/MathTypes.h>
#include <aymath/MathUtils.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace
{

constexpr int kWindowWidth  = 1280;
constexpr int kWindowHeight = 720;
constexpr int kShotFrames[] = {30, 60};
constexpr int kShotCount    = 2;

enum class DemoPath : int
{
    SolidRgba = 1,
    I420Grey  = 2,
};

struct DemoState
{
    ayt::game::GameLoop* loop = nullptr;
    bool running = true;
    int  frame = 0;
    int  frameCap = 0;
    DemoPath path = DemoPath::SolidRgba;
    std::string screenshotDir;

    ayt::render::MeshHandle     quad{};
    ayt::render::MaterialHandle material{};
    std::unique_ptr<ayt::video::IVideoFrameTexture> frameTex;
    ayt::render::DrawPayload2D  payload{};
    bool gpuReady = false;
};

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

void uploadDemoFrame(DemoState& state, ayt::render::Renderer& renderer)
{
    if (!state.frameTex)
    {
        state.frameTex = ayt::video::makeRendererVideoFrameTexture(renderer);
        if (!state.frameTex)
        {
            std::fprintf(stderr, "[AYVideoDemo] makeRendererVideoFrameTexture failed\n");
            return;
        }
    }

    std::vector<uint8_t> storage;
    ayt::video::VideoFrame frame{};
    if (state.path == DemoPath::I420Grey)
    {
        frame = makeI420Grey(storage, 64, 36, 128);
    }
    else
    {
        frame = makeSolidRgba(storage, 64, 36, 255, 0, 255, 255);
    }

    const ayt::video::VideoResult r = state.frameTex->updateFromFrame(frame);
    if (r != ayt::video::VideoResult::Ok)
    {
        std::fprintf(stderr, "[AYVideoDemo] updateFromFrame failed (%d)\n",
                     static_cast<int>(r));
        return;
    }

    auto* gpu = dynamic_cast<ayt::video::RendererVideoFrameTexture*>(
        state.frameTex.get());
    if (gpu == nullptr || !gpu->gpuHandle().isValid() || !state.material.isValid())
    {
        return;
    }
    renderer.setMaterialTexture(state.material, "albedoMap", gpu->gpuHandle());
    state.frameTex->clearDirty();
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
    uploadDemoFrame(state, renderer);
    state.payload.sourceRectMin = ayt::math::FVector2(0.0f, 0.0f);
    state.payload.sourceRectMax = ayt::math::FVector2(1.0f, 1.0f);
    state.payload.tintRGBA = ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f);
    state.gpuReady = state.frameTex && state.frameTex->gpuTextureId() != 0;
    std::fprintf(stderr, "[AYVideoDemo] GPU ready=%d texId=%llu path=%d\n",
                 state.gpuReady ? 1 : 0,
                 static_cast<unsigned long long>(
                     state.frameTex ? state.frameTex->gpuTextureId() : 0),
                 static_cast<int>(state.path));
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

    // Ortho covers the window in pixels; unit quad scaled to 640x360.
    const float halfH = static_cast<float>(kWindowHeight) * 0.5f;
    const float halfW = static_cast<float>(kWindowWidth) * 0.5f;
    renderer.setMainCamera(ayt::math::Float4x4::identity(),
                           ayt::math::ortho(-halfW, halfW, -halfH, halfH,
                                            -1.0f, 1.0f));

    ayt::render::DrawItem item;
    item.mesh = state.quad;
    item.material = state.material;
    item.payload = &state.payload;
    item.shadowFlags = ayt::render::ShadowFlags::None;
    item.world = ayt::math::Transform::getMatrix(
        ayt::math::FVector3(0.0f, 0.0f, 0.0f),
        ayt::math::FQuaternion::identity(),
        ayt::math::FVector3(640.0f, 360.0f, 1.0f));
    scene.add(item);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DemoState* state =
        reinterpret_cast<DemoState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
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
    wc.lpszClassName = L"AYVideoEngineDemo";
    RegisterClassExW(&wc);

    RECT rect{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"AYVideo Engine Demo — GPU frame texture",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);
    if (hwnd != nullptr)
    {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
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
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
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

    ayt::render::RendererSubSystem::setBootstrapWindow(
        hwnd, static_cast<uint32_t>(kWindowWidth),
        static_cast<uint32_t>(kWindowHeight));

    std::fprintf(stderr, "[AYVideoDemo] shaderc hint: %s (exists=%d)\n",
                 AY_SHADER_SHADERC_HINT,
                 fileExists(AY_SHADER_SHADERC_HINT) ? 1 : 0);
    std::fprintf(stderr,
                 "[AYVideoDemo] path=%d | Esc quit | shots at 30/60 | "
                 "AYVIDEO_DEMO_FRAMES=n auto-exit | shots=%s\n",
                 static_cast<int>(state.path), state.screenshotDir.c_str());

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

    char envFrames[16] = {};
    if (GetEnvironmentVariableA("AYVIDEO_DEMO_FRAMES", envFrames,
                                static_cast<DWORD>(sizeof(envFrames))) > 0)
    {
        state.frameCap = std::atoi(envFrames);
    }

    const uint64_t listenerId = loop.onUpdate([&](float /*dt*/) {
        pumpWin32Messages(state);
        if (!state.running)
        {
            loop.stop();
            return;
        }

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
                else
                {
                    std::fprintf(stderr,
                                 "[AYVideoDemo] frame %d captureScreenshot failed\n",
                                 state.frame);
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
        state.frame++;

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        {
            state.running = false;
            loop.stop();
        }
    });

    loop.run();
    loop.offUpdate(listenerId);
    loop.shutdown();

    std::fprintf(stderr, "[AYVideoDemo] done. frames=%d gpuReady=%d\n",
                 state.frame, state.gpuReady ? 1 : 0);
    std::printf("AYVideo_EngineDemo finished.\n");
    return state.gpuReady ? 0 : 2;
}
