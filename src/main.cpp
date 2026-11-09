#include <windows.h>
#include <d3d9.h>
#include <string.h>
#include <dwmapi.h>
#include <cmath>
#include <vector>
#include <string>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include "font8x8.h"

static LPDIRECT3D9           g_pD3D = NULL;
static LPDIRECT3DDEVICE9     g_pd3dDevice = NULL;
static D3DPRESENT_PARAMETERS g_d3dpp = {};
static bool                  g_DeviceLost = false;
static UINT                  g_ResizeWidth = 0, g_ResizeHeight = 0;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct Row {
    std::string text;
    bool prompt;
};

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

bool ConfigureSerialPort(HANDLE hSerial) {
    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(hSerial, &dcbSerialParams))
        return false;

    dcbSerialParams.BaudRate = CBR_115200;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    dcbSerialParams.fDtrControl = DTR_CONTROL_ENABLE;

    if (!SetCommState(hSerial, &dcbSerialParams))
        return false;

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = 20;
    timeouts.ReadTotalTimeoutConstant = 40;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 20;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(hSerial, &timeouts);

    return true;
}

HANDLE ScanAndConnectPico(std::string& foundPortName) {
    char portPath[32];
    for (int i = 1; i <= 32; i++) {
        sprintf(portPath, "\\\\.\\COM%d", i);

        HANDLE hSerial = CreateFileA(portPath, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        if (hSerial == INVALID_HANDLE_VALUE)
            continue;

        if (!ConfigureSerialPort(hSerial)) {
            CloseHandle(hSerial);
            continue;
        }

        PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

        const char* ping = "IDENTIFY\r\n";
        DWORD written = 0;
        WriteFile(hSerial, ping, (DWORD)strlen(ping), &written, NULL);

        Sleep(35);

        char readBuf[64] = { 0 };
        DWORD readBytes = 0;
        ReadFile(hSerial, readBuf, sizeof(readBuf) - 1, &readBytes, NULL);

        if (readBytes > 0 && strstr(readBuf, "PICO_W_OK") != NULL) {
            char displayPort[16];
            sprintf(displayPort, "COM%d", i);
            foundPortName = displayPort;

            COMMTIMEOUTS nonBlockingTimeouts = { MAXDWORD, 0, 0, 0, 0 };
            SetCommTimeouts(hSerial, &nonBlockingTimeouts);
            return hSerial;
        }

        CloseHandle(hSerial);
    }
    return INVALID_HANDLE_VALUE;
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

    char iconPath[MAX_PATH];
    GetModuleFileNameA(NULL, iconPath, MAX_PATH);
    char* slash = strrchr(iconPath, '\\');
    if (slash) {
        *(slash + 1) = '\0';
        strncat(iconPath, "assets\\icon.ico", sizeof(iconPath) - strlen(iconPath) - 1);
    }

    HICON hIconBig = (HICON)LoadImageA(NULL, iconPath, IMAGE_ICON,
                                       GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
    HICON hIconSmall = (HICON)LoadImageA(NULL, iconPath, IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);

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

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

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

    const size_t COLS = 36;
    const size_t MAXROWS = 19;
    const size_t MAXHISTORY = 200;

    std::vector<std::string> history;
    std::string currentLine = "";
    size_t cursorPos = 0;
    std::string incomingBuffer = "";

    std::string connectedPort = "";
    HANDLE hSerial = INVALID_HANDLE_VALUE;
    double lastScanTime = -10.0;

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

        double currentTime = ImGui::GetTime();

        if (hSerial == INVALID_HANDLE_VALUE) {
            if (currentTime - lastScanTime >= 1.5) {
                lastScanTime = currentTime;
                hSerial = ScanAndConnectPico(connectedPort);
                if (hSerial != INVALID_HANDLE_VALUE) {
                    history.clear();
                    currentLine.clear();
                    cursorPos = 0;
                    incomingBuffer.clear();
                }
            }
        } else {
            DWORD commErrors = 0;
            COMSTAT commStat;
            if (!ClearCommError(hSerial, &commErrors, &commStat)) {
                CloseHandle(hSerial);
                hSerial = INVALID_HANDLE_VALUE;
            } else {
                char chunk[128];
                DWORD bytesRead = 0;
                BOOL ok = ReadFile(hSerial, chunk, sizeof(chunk) - 1, &bytesRead, NULL);
                if (!ok) {
                    CloseHandle(hSerial);
                    hSerial = INVALID_HANDLE_VALUE;
                } else if (bytesRead > 0) {
                    chunk[bytesRead] = '\0';
                    incomingBuffer.append(chunk, bytesRead);

                    size_t nl;
                    while ((nl = incomingBuffer.find('\n')) != std::string::npos) {
                        std::string line = incomingBuffer.substr(0, nl);
                        if (!line.empty() && line.back() == '\r')
                            line.pop_back();

                        incomingBuffer.erase(0, nl + 1);

                        if (!line.empty()) {
                            history.push_back(line);
                            while (history.size() > MAXHISTORY)
                                history.erase(history.begin());
                        }
                    }
                }
            }
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (hSerial != INVALID_HANDLE_VALUE) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
                const char* clipText = ImGui::GetClipboardText();
                if (clipText != nullptr) {
                    while (*clipText) {
                        char c = *clipText++;
                        if (c == '\r' || c == '\n') break;
                        if (c >= 'a' && c <= 'z') c -= 32;
                        if (c >= 0x20 && c <= 0x7E) {
                            currentLine.insert(cursorPos, 1, c);
                            cursorPos++;
                        }
                    }
                }
            }

            for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
                unsigned int c = io.InputQueueCharacters[i];
                if (c >= 'a' && c <= 'z') c -= 32;
                if (c >= 0x20 && c <= 0x7E) {
                    currentLine.insert(cursorPos, 1, (char)c);
                    cursorPos++;
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                if (cursorPos > 0)
                    cursorPos--;
            }

            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
                if (cursorPos < currentLine.length())
                    cursorPos++;
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && cursorPos > 0) {
                currentLine.erase(cursorPos - 1, 1);
                cursorPos--;
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                currentLine.clear();
                cursorPos = 0;
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                if (currentLine == "CLEAR") {
                    history.clear();
                } else {
                    history.push_back(currentLine);
                    std::string commandToSend = currentLine + "\r\n";
                    DWORD bytesWritten;
                    BOOL ok = WriteFile(hSerial, commandToSend.c_str(), (DWORD)commandToSend.length(), &bytesWritten, NULL);
                    if (!ok) {
                        CloseHandle(hSerial);
                        hSerial = INVALID_HANDLE_VALUE;
                    }
                    while (history.size() > MAXHISTORY)
                        history.erase(history.begin());
                }
                currentLine.clear();
                cursorPos = 0;
            }
        }

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));

        ImGui::Begin("Display", nullptr, flags);
        {
            const float scale = 1.0f;
            const ImU32 white = IM_COL32(255, 255, 255, 255);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 o(floorf(vp->WorkPos.x + 8.0f), floorf(vp->WorkPos.y + 8.0f));

            if (hSerial == INVALID_HANDLE_VALUE) {
                Font8x8::Draw(dl, ImVec2(o.x, o.y), "FINDING ", scale, white);
                Font8x8::DrawSpinner(dl, ImVec2(o.x + 64.0f * scale, o.y), scale, white, 6.0f);
            } else {
                std::vector<Row> rows;
                for (const auto& entry : history) {
                    if (entry.empty()) {
                        rows.push_back({ "", true });
                        continue;
                    }
                    for (size_t pos = 0; pos < entry.length(); pos += COLS)
                        rows.push_back({ entry.substr(pos, COLS), pos == 0 });
                }

                size_t inputStartRow = rows.size();

                if (currentLine.empty()) {
                    rows.push_back({ "", true });
                } else {
                    for (size_t pos = 0; pos < currentLine.length(); pos += COLS)
                        rows.push_back({ currentLine.substr(pos, COLS), pos == 0 });
                    if (currentLine.length() % COLS == 0)
                        rows.push_back({ "", false });
                }

                size_t startRow = rows.size() > MAXROWS ? rows.size() - MAXROWS : 0;
                size_t cursorRowIndex = inputStartRow + (cursorPos / COLS);
                size_t cursorCol = cursorPos % COLS;

                float yOffset = 0.0f;
                for (size_t i = startRow; i < rows.size(); i++) {
                    if (rows[i].prompt)
                        Font8x8::Draw(dl, ImVec2(o.x, o.y + yOffset), ">", scale, white);
                    if (!rows[i].text.empty())
                        Font8x8::Draw(dl, ImVec2(o.x + 16.0f * scale, o.y + yOffset), rows[i].text.c_str(), scale, white);

                    if (i == cursorRowIndex && ((int)(ImGui::GetTime() * 2.0)) % 2 == 0)
                        Font8x8::Draw(dl, ImVec2(o.x + (16.0f + (float)cursorCol * 8.0f) * scale, o.y + yOffset), "_", scale, white);

                    yOffset += 12.0f * scale;
                }
            }
        }
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

    if (hIconBig) DestroyIcon(hIconBig);
    if (hIconSmall) DestroyIcon(hIconSmall);

    if (hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(hSerial);
    }

    return 0;
}
