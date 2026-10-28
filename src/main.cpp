#include <windows.h>
#include <d3d9.h>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"

static LPDIRECT3D9           g_pD3D = NULL;
static LPDIRECT3DDEVICE9     g_pd3dDevice = NULL;
static D3DPRESENT_PARAMETERS g_d3dpp = {};
static bool                  g_DeviceLost = false;
static UINT                  g_ResizeWidth = 0, g_ResizeHeight = 0;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool CreateDevice(HWND hwnd) {
    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D)
        return false;

    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferWidth = 320;
    g_d3dpp.BackBufferHeight = 240;
    g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_d3dpp.EnableAutoDepthStencil = FALSE;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                             D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp, &g_pd3dDevice) >= 0)
        return true;
    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                             D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_d3dpp, &g_pd3dDevice) >= 0)
        return true;

    g_pD3D->Release();
    g_pD3D = NULL;
    return false;
}

static void CleanupDevice() {
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
    if (g_pD3D)       { g_pD3D->Release();       g_pD3D = NULL; }
}

static bool ResetDevice() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
    if (FAILED(hr))
        return false;
    ImGui_ImplDX9_CreateDeviceObjects();
    return true;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_ResizeWidth = LOWORD(lParam);
                g_ResizeHeight = HIWORD(lParam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU)
                return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    ImGui_ImplWin32_EnableDpiAwareness();

    HICON hIconBig = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON hIconSmall = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0L, 0L, hInstance, hIconBig, NULL, NULL, NULL, "Armax-Display", hIconSmall };
    RegisterClassExA(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr = { 0, 0, 320, 240 };
    AdjustWindowRect(&wr, style, FALSE);

    HWND hwnd = CreateWindowA(wc.lpszClassName, "Armax-Display", style, 100, 100,
                              wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

    if (!CreateDevice(hwnd)) {
        DestroyWindow(hwnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    const D3DCOLOR clear_col = D3DCOLOR_RGBA(15, 15, 15, 255);

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        if (g_DeviceLost) {
            HRESULT hr = g_pd3dDevice->TestCooperativeLevel();
            if (hr == D3DERR_DEVICELOST) {
                Sleep(10);
                continue;
            }
            if (hr == D3DERR_DEVICENOTRESET) {
                if (!ResetDevice()) {
                    Sleep(10);
                    continue;
                }
            }
            g_DeviceLost = false;
        }

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            g_d3dpp.BackBufferWidth = g_ResizeWidth;
            g_d3dpp.BackBufferHeight = g_ResizeHeight;
            g_ResizeWidth = g_ResizeHeight = 0;
            if (!ResetDevice()) {
                g_DeviceLost = true;
                continue;
            }
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));

        ImGui::Begin("Display", nullptr, flags);
        ImGui::End();

        ImGui::PopStyleVar(2);

        ImGui::Render();

        g_pd3dDevice->Clear(0, NULL, D3DCLEAR_TARGET, clear_col, 1.0f, 0);
        if (g_pd3dDevice->BeginScene() >= 0) {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_pd3dDevice->EndScene();
        }

        HRESULT result = g_pd3dDevice->Present(NULL, NULL, NULL, NULL);
        if (result == D3DERR_DEVICELOST)
            g_DeviceLost = true;
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDevice();
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    return 0;
}
