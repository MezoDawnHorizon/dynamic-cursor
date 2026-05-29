#include <windows.h>
#include <d2d1.h>
#include <cmath>

// Link the Direct2D library automatically in Visual Studio
#pragma comment(lib, "d2d1.lib")

// --- Global Variables ---
ID2D1Factory* pFactory = nullptr;
ID2D1HwndRenderTarget* pRenderTarget = nullptr;
ID2D1SolidColorBrush* pBrush = nullptr;

// Physics States
float curX = 0.0f, curY = 0.0f;   // Dynamic cursor position
float targetX = 0.0f, targetY = 0.0f; // Real hardware mouse position
float lastAngle = 0.0f;           // Stores last angle to prevent flipping when stopped

// --- Direct2D Initialization ---
HRESULT InitD2D(HWND hwnd) {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pFactory);
    if (SUCCEEDED(hr)) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        
        // Create the render target
        hr = pFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, size),
            &pRenderTarget
        );
    }
    if (SUCCEEDED(hr)) {
        // Create a neon cyan brush for our custom cursor
        hr = pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 1.0f, 1.0f), &pBrush);
    }
    return hr;
}

// --- Direct2D Cleanup ---
void CleanUpD2D() {
    if (pBrush) pBrush->Release();
    if (pRenderTarget) pRenderTarget->Release();
    if (pFactory) pFactory->Release();
}

// --- The Core Physics & Render Engine ---
void RenderCursor() {
    if (!pRenderTarget) return;

    pRenderTarget->BeginDraw();
    
    // Clear the background with pure black. 
    // Because of our window settings, Windows will make pure black 100% transparent.
    pRenderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));

    // 1. Get real mouse coordinates
    POINT pt;
    GetCursorPos(&pt);
    targetX = static_cast<float>(pt.x);
    targetY = static_cast<float>(pt.y);

    // 2. Physics: Linear Interpolation (Lerp) for elastic lag
    float dx = targetX - curX;
    float dy = targetY - curY;
    
    float stiffness = 0.18f; // Higher = snappier, Lower = more lag/elasticity
    curX += dx * stiffness;
    curY += dy * stiffness;

    // 3. Physics: Velocity & Direction Calculations
    float vx = dx * stiffness;
    float vy = dy * stiffness;
    float speed = std::sqrt(vx * vx + vy * vy);

    // Calculate rotation angle if the mouse is actually moving
    float angle = lastAngle;
    if (speed > 0.5f) {
        angle = std::atan2(vy, vx) * (180.0f / 3.14159265f); // Convert radians to degrees
        lastAngle = angle;
    }

    // 4. Physics: Elastic Stretch Mode
    // Stretches along X based on speed, shrinks Y to maintain visual mass
    float stretchX = 1.0f + (speed * 0.04f); 
    float stretchY = 1.0f / stretchX; 
    
    // Cap maximum stretching so it doesn't break reality
    if (stretchX > 3.0f) { stretchX = 3.0f; stretchY = 1.0f / 3.0f; }

    // 5. Transformation Matrix Math (Order matters!)
    // We scale first, rotate second, and translate to the cursor position last.
    D2D1::Matrix3x2F transform = 
        D2D1::Matrix3x2F::Scale(stretchX, stretchY, D2D1::Point2F(0, 0)) *
        D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F(0, 0)) *
        D2D1::Matrix3x2F::Translation(curX, curY);

    pRenderTarget->SetTransform(transform);

    // 6. Draw the shape centered at (0,0)
    // Basic base size: 10px radius horizontal, 7px vertical
    D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(0, 0), 10.0f, 7.0f);
    pRenderTarget->FillEllipse(ellipse, pBrush);

    pRenderTarget->EndDraw();
}

// --- Standard Windows Event Handler ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_DESTROY:
            // Always remove window properties before the window destroys completely
            RemovePropW(hwnd, L"NonRudeHWND");
            CleanUpD2D();
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { // Press ESC to safely exit app
                DestroyWindow(hwnd);
            }
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// --- Application Entry Point ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Register the Window Class
    const wchar_t CLASS_NAME[] = L"CursorOverlayClass";
    
    WNDCLASSW wc = {}; 
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc); 

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    // Create a borderless, transparent, click-through, topmost overlay
    // ADDED: WS_EX_TOOLWINDOW (hides from Alt+Tab) & WS_EX_NOACTIVATE (prevents focus stealing)
    HWND hwnd = CreateWindowExW( 
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 
        CLASS_NAME, L"Dynamic Cursor Overlay",
        WS_POPUP, 
        0, 0, screenWidth, screenHeight,
        nullptr, nullptr, hInstance, nullptr
    );

    if (hwnd == nullptr) return 0;

    // FIX: Tells the OS "Rude Window Manager" that this screen-sized window 
    // is definitely not a fullscreen game, forcing the taskbar to stay on top.
    SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));

    // Chroma key setup: Tells Windows that pure black (RGB 0,0,0) should be invisible
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    if (FAILED(InitD2D(hwnd))) return 0;

    ShowWindow(hwnd, nCmdShow);

    // --- The Main Real-Time Game Loop ---
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            RenderCursor();
            Sleep(2); // Prevents maxing out your CPU core (~500 FPS ceiling)
        }
    }

    return 0;
}