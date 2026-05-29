#include <windows.h>
#include <d2d1.h>
#include <wincodec.h> // WIC header for handling image files
#include <cmath>

// Link the required Windows subsystem libraries automatically
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// --- Engine Configuration Settings ---
enum SimulationMode { MODE_STRETCH, MODE_ROTATE };

const SimulationMode CURRENT_MODE = MODE_STRETCH; // Choose: MODE_STRETCH or MODE_ROTATE
const float HOTSPOT_X = 4.0f;                     // X-offset to calibrate your cursor's true pointing tip
const float HOTSPOT_Y = 4.0f;                     // Y-offset to calibrate your cursor's true pointing tip
const float CURSOR_BASE_ANGLE = 45.0f;            // Rotate adjustment (Bibata naturally tilts at -45 deg)

// --- Global Variables ---
ID2D1Factory* pFactory = nullptr;
ID2D1HwndRenderTarget* pRenderTarget = nullptr;
ID2D1SolidColorBrush* pBrush = nullptr;
ID2D1Bitmap* pCursorBitmap = nullptr;             // Holds our loaded Bibata texture

// Physics States
float curX = 0.0f, curY = 0.0f;   
float targetX = 0.0f, targetY = 0.0f; 
float lastAngle = 0.0f;           

// --- WIC Texture Loader Implementation ---
void LoadCursorTexture() {
    IWICImagingFactory* pWICFactory = nullptr;
    IWICBitmapDecoder* pDecoder = nullptr;
    IWICBitmapFrameDecode* pFrame = nullptr;
    IWICFormatConverter* pConverter = nullptr;

    // Initialize the Windows Imaging Component Factory
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWICFactory)
    );
    
    if (SUCCEEDED(hr)) {
        // Attempt to open the local "cursor.png" file
        hr = pWICFactory->CreateDecoderFromFilename(
            L"cursor.png", nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDecoder
        );
    }
    if (SUCCEEDED(hr)) {
        hr = pDecoder->GetFrame(0, &pFrame);
    }
    if (SUCCEEDED(hr)) {
        hr = pWICFactory->CreateFormatConverter(&pConverter);
    }
    if (SUCCEEDED(hr)) {
        // Force the texture data to convert into a Direct2D standard Alpha-Premultiplied format
        hr = pConverter->Initialize(
            pFrame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom
        );
    }
    if (SUCCEEDED(hr)) {
        // Generate the high performance GPU bitmap frame
        pRenderTarget->CreateBitmapFromWicBitmap(pConverter, nullptr, &pCursorBitmap);
    }

    // Safely dispose raw pipeline layers
    if (pConverter) pConverter->Release();
    if (pFrame) pFrame->Release();
    if (pDecoder) pDecoder->Release();
    if (pWICFactory) pWICFactory->Release();
}

// --- Direct2D Initialization ---
HRESULT InitD2D(HWND hwnd) {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pFactory);
    if (SUCCEEDED(hr)) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        
        hr = pFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, size),
            &pRenderTarget
        );
    }
    if (SUCCEEDED(hr)) {
        hr = pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 1.0f, 1.0f), &pBrush);
        // Load our custom Bibata cursor asset into the renderer memory
        LoadCursorTexture();
    }
    return hr;
}

void CleanUpD2D() {
    if (pCursorBitmap) pCursorBitmap->Release();
    if (pBrush) pBrush->Release();
    if (pRenderTarget) pRenderTarget->Release();
    if (pFactory) pFactory->Release();
}

// --- The Physics & Custom Texture Transformation Engine ---
void RenderCursor(HWND hwnd) {
    if (!pRenderTarget) return;

    pRenderTarget->BeginDraw();
    pRenderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));

    // 1. Get real mouse coordinates
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    targetX = static_cast<float>(pt.x);
    targetY = static_cast<float>(pt.y);

    // 2. Physics: Adaptive Linear Interpolation (Lerp) for elite precision
    float dx = targetX - curX;
    float dy = targetY - curY;
    float distance = std::sqrt(dx * dx + dy * dy);

    // Default trailing elasticity (higher = snappier, lower = more lag)
    float stiffness = 0.16f; 

    // --- ADAPTIVE SNAPPING ENGINE ---
    // If your mouse is within 2 pixels of the target, snap instantly to ensure 1:1 clicking precision.
    if (distance < 2.0f) {
        stiffness = 1.0f; 
    } 
    // If you are slowing down and within 15 pixels of a target, tighten the physics tightly
    // so the cursor doesn't drift or overshoot your target button.
    else if (distance < 15.0f) {
        stiffness = 0.50f; 
    }
    
    curX += dx * stiffness;
    curY += dy * stiffness;

    // 3. Physics: Velocity Calculations
    float vx = dx * stiffness;
    float vy = dy * stiffness;
    float speed = std::sqrt(vx * vx + vy * vy);

    float angle = lastAngle;
    if (speed > 0.5f) {
        angle = std::atan2(vy, vx) * (180.0f / 3.14159265f); 
        lastAngle = angle;
    }

    // 4. Comic Book Squish & Stretch calculations
    float stretchX = 1.0f + (speed * 0.035f); 
    float stretchY = 1.0f / stretchX; 
    if (stretchX > 2.5f) { stretchX = 2.5f; stretchY = 1.0f / 2.5f; }

    // 5. Transformation Matrix Math (Hyprland Simulation Engine Mode Routing)
    D2D1::Matrix3x2F transform;

    if (CURRENT_MODE == MODE_STRETCH) {
        // --- STRETCH MODE ---
        // Matrix Sandwich trick: Moves image to origin -> tilts canvas backward to align velocity vector -> 
        // stretches texture -> tilts canvas back forward -> places cursor at real coordinates.
        transform = 
            D2D1::Matrix3x2F::Translation(-HOTSPOT_X, -HOTSPOT_Y) *
            D2D1::Matrix3x2F::Rotation(-angle, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Scale(stretchX, stretchY, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Translation(curX, curY);
    } 
    else {
        // --- ROTATE MODE ---
        // Dynamically forces the cursor point vector to point straight into the direction of motion.
        transform = 
            D2D1::Matrix3x2F::Translation(-HOTSPOT_X, -HOTSPOT_Y) *
            D2D1::Matrix3x2F::Scale(stretchX, stretchY, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Rotation(angle + CURSOR_BASE_ANGLE, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Translation(curX, curY);
    }

    pRenderTarget->SetTransform(transform);

    // 6. Final Render Layer Draw
    if (pCursorBitmap) {
        // If 'cursor.png' was loaded successfully, render it instantly
        D2D1_SIZE_F size = pCursorBitmap->GetSize();
        pRenderTarget->DrawBitmap(pCursorBitmap, D2D1::RectF(0, 0, size.width, size.height));
    } else {
        // Elegant Fallback: Draw default neon vector shape if image is missing
        D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(0, 0), 10.0f, 7.0f);
        pRenderTarget->FillEllipse(ellipse, pBrush);
    }

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
    // Initialize COM Subsystem thread for WIC Image Decoding
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

    if (hwnd == nullptr) { CoUninitialize(); return 0; }

    SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT, 'Q');

    if (FAILED(InitD2D(hwnd))) { CoUninitialize(); return 0; }

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
    return 0;
}