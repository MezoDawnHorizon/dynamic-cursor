#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <mmsystem.h>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

enum SimulationMode { MODE_STRETCH, MODE_ROTATE };
const SimulationMode CURRENT_MODE = MODE_STRETCH; 
const float CURSOR_BASE_ANGLE = 45.0f;            

// --- Polymorphic Cursor State Architecture ---
struct CursorState {
    HCURSOR sysHandle;          // Native Windows ID tracking handle
    const wchar_t* filename;    // Local PNG asset path
    ID2D1Bitmap* bitmap;        // Unique GPU texture layer
    bool isCenterHotspot;       // True for I-Beams/Resizers, False for standard pointers
    float manualHotspotX;       // Custom X calibration offset
    float manualHotspotY;       // Custom Y calibration offset
};

// Define all primary interactive cursor states
const int CURSOR_COUNT = 7;
CursorState cursorMap[CURSOR_COUNT] = {
    { LoadCursor(nullptr, IDC_ARROW),       L"pointer.png", nullptr, false, 4.0f, 4.0f },
    { LoadCursor(nullptr, IDC_HAND),        L"link.png",    nullptr, false, 8.0f, 2.0f },
    { LoadCursor(nullptr, IDC_IBEAM),       L"text.png",    nullptr, true,  0.0f, 0.0f },
    { LoadCursor(nullptr, IDC_WAIT),        L"busy.png",    nullptr, true,  0.0f, 0.0f },
    { LoadCursor(nullptr, IDC_APPSTARTING), L"work.png",    nullptr, false, 4.0f, 4.0f },
    { LoadCursor(nullptr, IDC_SIZEWE),      L"horz.png",    nullptr, true,  0.0f, 0.0f },
    { LoadCursor(nullptr, IDC_SIZENS),      L"vert.png",    nullptr, true,  0.0f, 0.0f }
};

// --- Global Engine Variables ---
ID2D1Factory* pFactory = nullptr;
ID2D1HwndRenderTarget* pRenderTarget = nullptr;
ID2D1SolidColorBrush* pBrush = nullptr;

float curX = 0.0f, curY = 0.0f;   
float targetX = 0.0f, targetY = 0.0f; 
float lastAngle = 0.0f;           

// Reusable texture processing pipeline
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
        hr = pWICFactory->CreateFormatConverter(&pConverter);
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
        hr = pFactory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), D2D1::HwndRenderTargetProperties(hwnd, size), &pRenderTarget);
    }
    if (SUCCEEDED(hr)) {
        hr = pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 1.0f, 1.0f), &pBrush);
        
        // Loop and cache all valid PNG textures directly into high performance GPU VRAM
        for (int i = 0; i < CURSOR_COUNT; i++) {
            cursorMap[i].bitmap = LoadTextureFromFile(cursorMap[i].filename);
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

    pRenderTarget->BeginDraw();
    pRenderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));

    // 1. Polling System: Track and poll active kernel cursor handles
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    targetX = static_cast<float>(pt.x);
    targetY = static_cast<float>(pt.y);

    CURSORINFO ci = { sizeof(CURSORINFO) };
    CursorState activeCursor = cursorMap[0]; // Fallback cleanly to standard Arrow layout

    if (GetCursorInfo(&ci)) {
        for (int i = 0; i < CURSOR_COUNT; i++) {
            if (ci.hCursor == cursorMap[i].sysHandle) {
                activeCursor = cursorMap[i];
                break;
            }
        }
    }

    // Fallback security assertion: Check if target sub-texture asset exists
    ID2D1Bitmap* activeBitmap = activeCursor.bitmap ? activeCursor.bitmap : cursorMap[0].bitmap;
    if (!activeBitmap) { pRenderTarget->EndDraw(); return; }

    D2D1_SIZE_F size = activeBitmap->GetSize();
    float hX = activeCursor.isCenterHotspot ? (size.width / 2.0f) : activeCursor.manualHotspotX;
    float hY = activeCursor.isCenterHotspot ? (size.height / 2.0f) : activeCursor.manualHotspotY;

    // 2. Physics Configuration
    float dx = targetX - curX;
    float dy = targetY - curY;
    float distance = std::sqrt(dx * dx + dy * dy);

    float stiffness = 0.24f; 
    if (distance > 180.0f)      stiffness = 0.65f; 
    else if (distance > 60.0f)  stiffness = 0.42f; 
    else if (distance < 2.0f)   stiffness = 1.0f;  
    else if (distance < 15.0f)  stiffness = 0.68f; 
    
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

    float stretchX = 1.0f + (speed * 0.025f); 
    float stretchY = 1.0f / stretchX; 
    if (stretchX > 2.2f) { stretchX = 2.2f; stretchY = 1.0f / 2.2f; }

    // 3. Mathematical Transformation Processing
    D2D1::Matrix3x2F transform;
    if (CURRENT_MODE == MODE_STRETCH) {
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
            D2D1::Matrix3x2F::Scale(stretchX, stretchY, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Rotation(angle + CURSOR_BASE_ANGLE, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Translation(curX, curY);
    }

    pRenderTarget->SetTransform(transform);

    // 4. Draw Command Execution
    pRenderTarget->DrawBitmap(activeBitmap, D2D1::RectF(0, 0, size.width, size.height));
    pRenderTarget->EndDraw();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_HOTKEY:
            if (wParam == 1) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            UnregisterHotKey(hwnd, 1);
            RemovePropW(hwnd, L"NonRudeHWND");
            CleanUpD2D();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const wchar_t CLASS_NAME[] = L"CursorOverlayClass";
    WNDCLASSW wc = {}; 
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc); 

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW( 
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 
        CLASS_NAME, L"Dynamic Cursor Overlay", WS_POPUP, 
        0, 0, screenWidth, screenHeight, nullptr, nullptr, hInstance, nullptr
    );

    if (hwnd == nullptr) { CoUninitialize(); timeEndPeriod(1); return 0; }

    SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT, 'Q');

    if (FAILED(InitD2D(hwnd))) { CoUninitialize(); timeEndPeriod(1); return 0; }

    ShowWindow(hwnd, nCmdShow);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            RenderCursor(hwnd);
            Sleep(1); 
        }
    }

    CoUninitialize();
    timeEndPeriod(1); 
    return 0;
}