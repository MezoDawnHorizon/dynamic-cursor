#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <dwmapi.h> 
#include <mmsystem.h>
#include <shellapi.h>
#include <cmath>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT          1001
#define ID_TRAY_RELOAD        1002
#define ID_TRAY_MODE_STRETCH  1003
#define ID_TRAY_MODE_ROTATE   1004
#define TIMER_RENDER_MENU     1

enum SimulationMode { MODE_STRETCH = 0, MODE_ROTATE = 1 };

struct RuntimeConfig {
    SimulationMode currentMode = MODE_STRETCH;
    float stiffnessFar         = 0.65f;
    float stiffnessMedium      = 0.42f;
    float stiffnessClose       = 0.24f;
    float stretchFactor        = 0.025f;
    float maxStretch           = 2.2f;
} g_Config;

struct CursorState {
    HCURSOR sysHandle;          
    const wchar_t* filename;    
    ID2D1Bitmap* bitmap;        
    bool isCenterHotspot;       
    float manualHotspotX;       
    float manualHotspotY;       
    float baseAngle;      
    bool allowRotation;   
};

const int CURSOR_COUNT = 5;
CursorState cursorMap[CURSOR_COUNT] = {
    { LoadCursor(nullptr, IDC_ARROW),  L"pointer.png", nullptr, false, 4.0f, 4.0f, 135.0f, true  },
    { LoadCursor(nullptr, IDC_HAND),   L"link.png",    nullptr, false, 8.0f, 2.0f, 135.0f, true  },
    { LoadCursor(nullptr, IDC_IBEAM),  L"text.png",    nullptr, true,  0.0f, 0.0f, 0.0f,   false }, 
    { LoadCursor(nullptr, IDC_SIZEWE), L"horz.png",    nullptr, true,  0.0f, 0.0f, 0.0f,   false },
    { LoadCursor(nullptr, IDC_SIZENS), L"vert.png",    nullptr, true,  0.0f, 0.0f, 0.0f,   false }
};

ID2D1Factory* pFactory = nullptr;
ID2D1HwndRenderTarget* pRenderTarget = nullptr;
ID2D1SolidColorBrush* pBrush = nullptr;

float curX = 0.0f, curY = 0.0f;   
float targetX = 0.0f, targetY = 0.0f; 
float lastAngle = 0.0f;           

NOTIFYICONDATAW nid = {}; 
const wchar_t* CONFIG_DIR  = L"config";
const wchar_t* CONFIG_FILE = L".\\config\\settings.ini";

float GetINIFloat(const wchar_t* section, const wchar_t* key, float defaultValue, const wchar_t* filePath) {
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, std::to_wstring(defaultValue).c_str(), buf, 64, filePath);
    return std::wcstof(buf, nullptr);
}

void LoadOrWriteConfig() {
    CreateDirectoryW(CONFIG_DIR, nullptr);
    HANDLE hFile = CreateFileW(CONFIG_FILE, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        WritePrivateProfileStringW(L"Physics", L"SimulationMode", L"0", CONFIG_FILE);
        WritePrivateProfileStringW(L"Physics", L"StiffnessFar",    L"0.65", CONFIG_FILE);
        WritePrivateProfileStringW(L"Physics", L"StiffnessMedium", L"0.42", CONFIG_FILE);
        WritePrivateProfileStringW(L"Physics", L"StiffnessClose",  L"0.24", CONFIG_FILE);
        WritePrivateProfileStringW(L"Physics", L"StretchFactor",   L"0.025", CONFIG_FILE);
        WritePrivateProfileStringW(L"Physics", L"MaxStretch",      L"2.2", CONFIG_FILE);
    } else {
        CloseHandle(hFile);
    }

    g_Config.currentMode     = static_cast<SimulationMode>(GetPrivateProfileIntW(L"Physics", L"SimulationMode", 0, CONFIG_FILE));
    g_Config.stiffnessFar    = GetINIFloat(L"Physics", L"StiffnessFar", 0.65f, CONFIG_FILE);
    g_Config.stiffnessMedium = GetINIFloat(L"Physics", L"StiffnessMedium", 0.42f, CONFIG_FILE);
    g_Config.stiffnessClose  = GetINIFloat(L"Physics", L"StiffnessClose", 0.24f, CONFIG_FILE);
    g_Config.stretchFactor   = GetINIFloat(L"Physics", L"StretchFactor", 0.025f, CONFIG_FILE);
    g_Config.maxStretch      = GetINIFloat(L"Physics", L"MaxStretch", 2.2f, CONFIG_FILE);
}

void UpdateINIMode(SimulationMode mode) {
    g_Config.currentMode = mode;
    WritePrivateProfileStringW(L"Physics", L"SimulationMode", std::to_wstring(static_cast<int>(mode)).c_str(), CONFIG_FILE);
}

ID2D1Bitmap* LoadTextureFromFile(const wchar_t* filename) {
    IWICImagingFactory* pWICFactory = nullptr;
    IWICBitmapDecoder* pDecoder = nullptr;
    IWICBitmapFrameDecode* pFrame = nullptr;
    IWICFormatConverter* pConverter = nullptr;
    ID2D1Bitmap* pBitmap = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWICFactory));
    if (SUCCEEDED(hr)) {
        hr = pWICFactory->CreateDecoderFromFilename(filename, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDecoder);
    }
    if (SUCCEEDED(hr)) {
        hr = pDecoder->GetFrame(0, &pFrame);
    }
    if (SUCCEEDED(hr)) {
        pWICFactory->CreateFormatConverter(&pConverter);
    }
    if (SUCCEEDED(hr)) {
        hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) {
        pRenderTarget->CreateBitmapFromWicBitmap(pConverter, nullptr, &pBitmap);
    }

    if (pConverter) pConverter->Release();
    if (pFrame) pFrame->Release();
    if (pDecoder) pDecoder->Release();
    if (pWICFactory) pWICFactory->Release();

    return pBitmap;
}

HRESULT InitD2D(HWND hwnd) {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pFactory);
    if (SUCCEEDED(hr)) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        
        hr = pFactory->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, size), &pRenderTarget);
    }
    if (SUCCEEDED(hr)) {
        hr = pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 1.0f, 1.0f), &pBrush);
        
        for (int i = 0; i < CURSOR_COUNT; i++) {
            cursorMap[i].bitmap = LoadTextureFromFile(cursorMap[i].filename);
            if (!cursorMap[i].bitmap) {
                wchar_t errorMsg[256];
                wsprintfW(errorMsg, L"Missing Asset: %s\n\nPlease verify file layout.", cursorMap[i].filename);
                MessageBoxW(hwnd, errorMsg, L"Asset Read Error", MB_OK | MB_ICONERROR);
                return E_FAIL; 
            }
        }
    }
    return hr;
}

void CleanUpD2D() {
    for (int i = 0; i < CURSOR_COUNT; i++) {
        if (cursorMap[i].bitmap) { cursorMap[i].bitmap->Release(); cursorMap[i].bitmap = nullptr; }
    }
    if (pBrush) pBrush->Release();
    if (pRenderTarget) pRenderTarget->Release();
    if (pFactory) pFactory->Release();
}

void RenderCursor(HWND hwnd) {
    if (!pRenderTarget) return;

    // Force our overlay to assert dominance over aggressive context menus
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    pRenderTarget->BeginDraw();
    pRenderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)); 

    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    targetX = static_cast<float>(pt.x);
    targetY = static_cast<float>(pt.y);

    CURSORINFO ci = { sizeof(CURSORINFO) };
    bool foundMatchedCursor = false;
    bool cursorIsShowing = false;
    CursorState activeCursor;

    if (GetCursorInfo(&ci)) {
        if (ci.flags & CURSOR_SHOWING) {
            cursorIsShowing = true;
            for (int i = 0; i < CURSOR_COUNT; i++) {
                if (ci.hCursor == cursorMap[i].sysHandle) {
                    activeCursor = cursorMap[i];
                    foundMatchedCursor = true;
                    break;
                }
            }
            
            // Fallback to custom pointer instead of vanishing on unknown types
            if (!foundMatchedCursor) {
                activeCursor = cursorMap[0]; 
                foundMatchedCursor = true;
            }
        }
    }

    if (!cursorIsShowing || !activeCursor.bitmap) {
        curX = targetX;
        curY = targetY;
        pRenderTarget->EndDraw();
        DwmFlush();
        return; 
    }

    D2D1_SIZE_F size = activeCursor.bitmap->GetSize();
    float hX = activeCursor.isCenterHotspot ? (size.width / 2.0f) : activeCursor.manualHotspotX;
    float hY = activeCursor.isCenterHotspot ? (size.height / 2.0f) : activeCursor.manualHotspotY;

    float dx = targetX - curX;
    float dy = targetY - curY;
    float distance = std::sqrt(dx * dx + dy * dy);

    float stiffness = g_Config.stiffnessClose; 
    if (distance > 180.0f)      stiffness = g_Config.stiffnessFar; 
    else if (distance > 60.0f)  stiffness = g_Config.stiffnessMedium; 
    else if (distance < 2.0f)   stiffness = 1.0f;  
    else if (distance < 15.0f)  stiffness = g_Config.stiffnessClose * 2.833f; 
    
    curX += dx * stiffness;
    curY += dy * stiffness;

    float vx = dx * stiffness;
    float vy = dy * stiffness;
    float speed = std::sqrt(vx * vx + vy * vy);

    float angle = lastAngle;
    if (speed > 0.5f) {
        angle = std::atan2(vy, vx) * (180.0f / 3.14159265f); 
        lastAngle = angle;
    }

    float stretchX = 1.0f + (speed * g_Config.stretchFactor); 
    float stretchY = 1.0f / stretchX; 
    if (stretchX > g_Config.maxStretch) { 
        stretchX = g_Config.maxStretch; 
        stretchY = 1.0f / g_Config.maxStretch; 
    }

    D2D1::Matrix3x2F transform;
    
    if (g_Config.currentMode == MODE_STRETCH || !activeCursor.allowRotation) {
        transform = 
            D2D1::Matrix3x2F::Translation(-hX, -hY) *
            D2D1::Matrix3x2F::Rotation(-angle, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Scale(stretchX, stretchY, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Translation(curX, curY);
    } 
    else {
        transform = 
            D2D1::Matrix3x2F::Translation(-hX, -hY) *
            D2D1::Matrix3x2F::Rotation(activeCursor.baseAngle, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Scale(stretchX, stretchY, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Translation(curX, curY);
    }

    pRenderTarget->SetTransform(transform);
    pRenderTarget->DrawBitmap(activeCursor.bitmap, D2D1::RectF(0, 0, size.width, size.height));
    pRenderTarget->EndDraw();

    // Flushes render queue and limits execution rate perfectly to the display monitor refresh rate
    DwmFlush(); 
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP) {
                POINT curPoint;
                GetCursorPos(&curPoint);
                
                // 1. Fire up a fast window timer to drive RenderCursor while TrackPopupMenu blocks WinMain
                SetTimer(hwnd, TIMER_RENDER_MENU, 10, nullptr);

                HMENU hMenu = CreatePopupMenu();
                if (hMenu) {
                    AppendMenuW(hMenu, g_Config.currentMode == MODE_STRETCH ? MF_CHECKED : MF_UNCHECKED, ID_TRAY_MODE_STRETCH, L"Stretch Animation Mode");
                    AppendMenuW(hMenu, g_Config.currentMode == MODE_ROTATE ? MF_CHECKED : MF_UNCHECKED, ID_TRAY_MODE_ROTATE, L"Rotate Animation Mode");
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(hMenu, MF_STRING, ID_TRAY_RELOAD, L"Reload settings.ini");
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit Cursor Overlay");
                    
                    SetForegroundWindow(hwnd);
                    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, curPoint.x, curPoint.y, 0, hwnd, NULL);
                    DestroyMenu(hMenu);
                }

                // 2. Kill the timer immediately once the context menu closes and normal execution returns
                KillTimer(hwnd, TIMER_RENDER_MENU);
            }
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_RENDER_MENU) {
                // Windows feeds us these messages inside its own modal loops!
                RenderCursor(hwnd);
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    break;
                case ID_TRAY_RELOAD:
                    LoadOrWriteConfig();
                    break;
                case ID_TRAY_MODE_STRETCH:
                    UpdateINIMode(MODE_STRETCH);
                    break;
                case ID_TRAY_MODE_ROTATE:
                    UpdateINIMode(MODE_ROTATE);
                    break;
            }
            return 0;

        case WM_HOTKEY:
            if (wParam == 1) DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &nid);
            UnregisterHotKey(hwnd, 1);
            RemovePropW(hwnd, L"NonRudeHWND");
            CleanUpD2D();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    SetProcessDPIAware();
    LoadOrWriteConfig();
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const wchar_t CLASS_NAME[] = L"CursorOverlayClass";
    WNDCLASSW wc = {}; 
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc); 

    int virtualLeft   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtualTop    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int virtualWidth  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HWND hwnd = CreateWindowExW( 
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 
        CLASS_NAME, L"Dynamic Cursor Overlay", WS_POPUP, 
        virtualLeft, virtualTop, virtualWidth, virtualHeight, nullptr, nullptr, hInstance, nullptr
    );

    if (hwnd == nullptr) { CoUninitialize(); timeEndPeriod(1); return 0; }

    SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT, 'Q');

    if (FAILED(InitD2D(hwnd))) { CoUninitialize(); timeEndPeriod(1); return 0; }

    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION); 
    wcscpy_s(nid.szTip, L"Dynamic Cursor Overlay");
    Shell_NotifyIconW(NIM_ADD, &nid);

    ShowWindow(hwnd, nCmdShow);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            RenderCursor(hwnd); // Normal state loop
        }
    }

    CoUninitialize();
    timeEndPeriod(1); 
    return 0;
}