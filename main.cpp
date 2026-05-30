#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <dwmapi.h> 
#include <mmsystem.h>
#include <shellapi.h>
#include <commdlg.h>
#include <cmath>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

// SYSTEM MESSAGES & CONTROL IDS

#define WM_TRAYICON           (WM_USER + 1)
#define ID_TRAY_EXIT          1001
#define ID_TRAY_RELOAD        1002
#define ID_TRAY_MODE_STRETCH  1003
#define ID_TRAY_MODE_ROTATE   1004
#define ID_TRAY_SETTINGS      1005

#define IDC_COMBO_CURSOR      2001
#define IDC_BTN_BROWSE        2002

#define TIMER_RENDER_LOOP     1

// ============================================================================
// CORE DATA STRUCTURES
// ============================================================================

// Defines the two animation modes for cursor physics
enum SimulationMode { MODE_STRETCH = 0, MODE_ROTATE = 1 };

// Physics configuration loaded from settings.ini
// Controls how the cursor moves, stretches, and responds to velocity
struct RuntimeConfig {
    SimulationMode currentMode = MODE_STRETCH;
    float stiffnessFar         = 0.65f;
    float stiffnessMedium      = 0.42f;
    float stiffnessClose       = 0.24f;
    float stretchFactor        = 0.025f;
    float maxStretch           = 2.2f;
} g_Config;

// Represents a single cursor image with its properties and rotation/stretch settings
// Each cursor type (arrow, hand, text selection, etc.) has one CursorState entry
struct CursorState {
    HCURSOR sysHandle;          // System cursor handle this maps to (IDC_ARROW, IDC_HAND, etc.)
    const wchar_t* filename;    // Path to the custom PNG image file
    ID2D1Bitmap* bitmap;        // Loaded graphics bitmap (nullptr until texture is loaded)
    bool isCenterHotspot;       // If true, hotspot is center of image; if false, use manual coords
    float manualHotspotX;       // X offset of the click point from top-left
    float manualHotspotY;       // Y offset of the click point from top-left
    float baseAngle;            // Starting rotation angle (degrees) for ROTATE mode
    bool allowRotation;         // Whether this cursor should rotate with movement
};

const int CURSOR_COUNT = 5;
// Maps Windows system cursors to custom PNG files
// Array indices: 0=arrow, 1=hand, 2=text, 3=resize-h, 4=resize-v
CursorState cursorMap[CURSOR_COUNT] = {
    { LoadCursor(nullptr, IDC_ARROW),  L".\\config\\pointer.png", nullptr, false, 4.0f, 4.0f, 135.0f, true  },
    { LoadCursor(nullptr, IDC_HAND),   L".\\config\\link.png",    nullptr, false, 8.0f, 2.0f, 135.0f, true  },
    { LoadCursor(nullptr, IDC_IBEAM),  L".\\config\\text.png",    nullptr, true,  0.0f, 0.0f, 0.0f,   false }, 
    { LoadCursor(nullptr, IDC_SIZEWE), L".\\config\\horz.png",    nullptr, true,  0.0f, 0.0f, 0.0f,   false },
    { LoadCursor(nullptr, IDC_SIZENS), L".\\config\\vert.png",    nullptr, true,  0.0f, 0.0f, 0.0f,   false }
};

// ============================================================================
// GRAPHICS & RENDERING GLOBALS
// ============================================================================

ID2D1Factory* pFactory = nullptr; // Direct2D factory (creates rendering resources)
ID2D1HwndRenderTarget* pRenderTarget = nullptr; // Direct2D render target (window surface we draw to)
ID2D1SolidColorBrush* pBrush = nullptr; // Brush used for any solid color drawing (currently unused but kept for future features)

// ============================================================================
// CURSOR PHYSICS SIMULATION VARIABLES
// ============================================================================


float curX = 0.0f, curY = 0.0f; // Current rendered cursor position (smoothly follows targetX/targetY based on physics)
float targetX = 0.0f, targetY = 0.0f; // Where the actual mouse pointer is (goal position we're lerping toward)
float lastAngle = 0.0f; // Last calculated rotation angle (stored to avoid recalculating each frame)

// ============================================================================
// UI & SYSTEM STATE
// ============================================================================

// Window handle for the settings dialog (nullptr if closed)
HWND g_hwndSettings = nullptr; 
NOTIFYICONDATAW nid = {}; 
const wchar_t* CONFIG_DIR  = L"config";
const wchar_t* CONFIG_FILE = L".\\config\\settings.ini";

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Reads a floating-point value from an INI file section
// Falls back to defaultValue if key doesn't exist
float GetINIFloat(const wchar_t* section, const wchar_t* key, float defaultValue, const wchar_t* filePath) {
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, std::to_wstring(defaultValue).c_str(), buf, 64, filePath);
    return std::wcstof(buf, nullptr);
}

// ============================================================================
// CONFIGURATION MANAGEMENT
// ============================================================================

// Create config folder and load settings from settings.ini
// If the file doesn't exist, create it with default physics values
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

// Update the simulation mode in both memory and settings.ini file
// Changes take effect on the next rendered frame
void UpdateINIMode(SimulationMode mode) {
    g_Config.currentMode = mode;
    WritePrivateProfileStringW(L"Physics", L"SimulationMode", std::to_wstring(static_cast<int>(mode)).c_str(), CONFIG_FILE);
}

// ============================================================================
// GRAPHICS ASSET LOADING
// ============================================================================

// Load cursor image into memory so the file isn't locked by the graphics engine
// This allows hot-swapping cursor files without needing to restart
ID2D1Bitmap* LoadTextureFromFile(const wchar_t* filename) {
    IWICImagingFactory* pWICFactory = nullptr;
    IWICBitmapDecoder* pDecoder = nullptr;
    IWICBitmapFrameDecode* pFrame = nullptr;
    IWICFormatConverter* pConverter = nullptr;
    ID2D1Bitmap* pBitmap = nullptr;
    IStream* pStream = nullptr;

    // Open file without locking it (other programs can still modify it)
    HANDLE hFile = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return nullptr;

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        CloseHandle(hFile);
        return nullptr;
    }

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, fileSize);
    if (!hGlobal) {
        CloseHandle(hFile);
        return nullptr;
    }

    void* pData = GlobalLock(hGlobal);
    DWORD bytesRead = 0;
    if (pData) {
        ReadFile(hFile, pData, fileSize, &bytesRead, nullptr);
        GlobalUnlock(hGlobal);
    }
    CloseHandle(hFile);

    if (bytesRead != fileSize) {
        GlobalFree(hGlobal);
        return nullptr;
    }

    // Wrap the memory buffer as a file stream so the graphics library can read it
    if (SUCCEEDED(CreateStreamOnHGlobal(hGlobal, TRUE, &pStream))) {
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWICFactory));
        if (SUCCEEDED(hr)) {
            hr = pWICFactory->CreateDecoderFromStream(pStream, nullptr, WICDecodeMetadataCacheOnLoad, &pDecoder);
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
        if (SUCCEEDED(hr) && pRenderTarget) {
            pRenderTarget->CreateBitmapFromWicBitmap(pConverter, nullptr, &pBitmap);
        }

        if (pConverter) pConverter->Release();
        if (pFrame) pFrame->Release();
        if (pDecoder) pDecoder->Release();
        if (pWICFactory) pWICFactory->Release();
        pStream->Release(); // Automatically frees underlying HGLOBAL allocation context cleanly
    }

    return pBitmap;
}

// Reload a cursor texture by index
// Releases the old bitmap and loads the new one from disk
void ReloadCursorTexture(int index) {
    if (index < 0 || index >= CURSOR_COUNT) return;
    if (cursorMap[index].bitmap) {
        cursorMap[index].bitmap->Release();
        cursorMap[index].bitmap = nullptr;
    }
    cursorMap[index].bitmap = LoadTextureFromFile(cursorMap[index].filename);
}

// ============================================================================
// DIRECT2D GRAPHICS INITIALIZATION
// ============================================================================

// Initialize Direct2D graphics pipeline and load all cursor textures
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
        }
    }
    return hr;
}

// Release all Direct2D resources and cursor bitmaps
void CleanUpD2D() {
    for (int i = 0; i < CURSOR_COUNT; i++) {
        if (cursorMap[i].bitmap) { cursorMap[i].bitmap->Release(); cursorMap[i].bitmap = nullptr; }
    }
    if (pBrush) pBrush->Release();
    if (pRenderTarget) pRenderTarget->Release();
    if (pFactory) pFactory->Release();
}

// ============================================================================
// MAIN RENDERING FUNCTION - CURSOR PHYSICS SIMULATION
// ============================================================================

// Core rendering logic: updates cursor position, applies physics, and draws it
// Called every frame (synced to monitor refresh rate via SetTimer)
void RenderCursor(HWND hwnd) {
    if (!pRenderTarget) return;

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    pRenderTarget->BeginDraw();
    pRenderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // Get current mouse position in screen coordinates
    // Convert to window client coordinates for rendering
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    targetX = static_cast<float>(pt.x);
    targetY = static_cast<float>(pt.y);

    // Get current system cursor info to determine which custom cursor to use
    CURSORINFO ci = { sizeof(CURSORINFO) };
    bool foundMatchedCursor = false;
    bool cursorIsShowing = false;
    CursorState activeCursor;

    if (GetCursorInfo(&ci)) {
        if (ci.flags & CURSOR_SHOWING) {
            cursorIsShowing = true;
            // Find which custom cursor matches the current system cursor
            for (int i = 0; i < CURSOR_COUNT; i++) {
                if (ci.hCursor == cursorMap[i].sysHandle) {
                    activeCursor = cursorMap[i];
                    foundMatchedCursor = true;
                    break;
                }
            }
            // Note: We only draw custom cursors we recognize. Unmapped system cursors (like hourglass, hand, etc.) are hidden
        }
    }

    // If Windows hides the cursor or uses a system cursor we don't have a custom version for,
    // let Windows draw its native cursor instead of our overlay (better than drawing nothing)
    if (!cursorIsShowing || !foundMatchedCursor || !activeCursor.bitmap) {
        curX = targetX;
        curY = targetY;
        pRenderTarget->EndDraw();
        DwmFlush();
        return; 
    }

    // Calculate hotspot (the click point of the cursor)
    // Some cursors center on the image, others have a specific manual offset
    D2D1_SIZE_F size = activeCursor.bitmap->GetSize();
    float hX = activeCursor.isCenterHotspot ? (size.width / 2.0f) : activeCursor.manualHotspotX;
    float hY = activeCursor.isCenterHotspot ? (size.height / 2.0f) : activeCursor.manualHotspotY;

    // Calculate how far the rendered cursor is from the actual mouse position
    // This is used for physics simulation
    float dx = targetX - curX;
    float dy = targetY - curY;
    float distance = std::sqrt(dx * dx + dy * dy);

    // Adaptive physics: adjust how fast the cursor catches up based on distance
    // Close movements are snappy, far movements are smooth and elastic
    float stiffness = g_Config.stiffnessClose; 
    if (distance > 180.0f)      stiffness = g_Config.stiffnessFar;      // Very far: use maximum lerp speed
    else if (distance > 60.0f)  stiffness = g_Config.stiffnessMedium;  // Medium distance: moderate speed
    else if (distance < 2.0f)   stiffness = 1.0f;                       // Extremely close: instant positioning
    else if (distance < 15.0f)  stiffness = g_Config.stiffnessClose * 2.833f;  // Very close: smoother transition
    
    // Apply physics: move rendered cursor toward target position
    curX += dx * stiffness;
    curY += dy * stiffness;

    // Calculate velocity (how fast the cursor is moving)
    float vx = dx * stiffness;
    float vy = dy * stiffness;
    float speed = std::sqrt(vx * vx + vy * vy);

    // Calculate rotation angle based on movement direction
    // Only recalculate if moving fast enough to avoid noise
    float angle = lastAngle;
    if (speed > 0.5f) {
        angle = std::atan2(vy, vx) * (180.0f / 3.14159265f);  // Convert radians to degrees
        lastAngle = angle;
    }

    // Calculate stretch deformation based on velocity
    // Stretches in direction of movement, compresses perpendicular to movement
    float stretchX = 1.0f + (speed * g_Config.stretchFactor);
    float stretchY = 1.0f / stretchX;  // Maintain volume by inverse scaling
    // Clamp stretch to prevent extreme distortion
    if (stretchX > g_Config.maxStretch) {
        stretchX = g_Config.maxStretch;
        stretchY = 1.0f / g_Config.maxStretch;
    }

    // Build transformation matrix based on simulation mode
    // Transform: translate to origin -> rotate -> stretch -> rotate back -> translate to final position
    D2D1::Matrix3x2F transform;
    
    if (g_Config.currentMode == MODE_STRETCH || !activeCursor.allowRotation) {
        // STRETCH MODE: deform the image in direction of movement
        transform = 
            D2D1::Matrix3x2F::Translation(-hX, -hY) *
            D2D1::Matrix3x2F::Rotation(-angle, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Scale(stretchX, stretchY, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Translation(curX, curY);
    } 
    else {
        // ROTATE MODE: spin the entire image toward movement direction
        transform = 
            D2D1::Matrix3x2F::Translation(-hX, -hY) *
            D2D1::Matrix3x2F::Rotation(activeCursor.baseAngle, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Scale(stretchX, stretchY, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F(0, 0)) *
            D2D1::Matrix3x2F::Translation(curX, curY);
    }

    // Apply transformation and render the cursor bitmap
    pRenderTarget->SetTransform(transform);
    pRenderTarget->DrawBitmap(activeCursor.bitmap, D2D1::RectF(0, 0, size.width, size.height));
    pRenderTarget->EndDraw();
    // Flush to ensure rendered frame is displayed immediately
    DwmFlush(); 
}

// ============================================================================
// SETTINGS GUI & FILE IMPORT
// ============================================================================

// Handle dragged/selected cursor image files
// Copies file to config folder and applies it to the selected cursor type
void ProcessImportedFile(HWND hwndSettings, const wchar_t* sourcePath) {
    HWND hwndCombo = GetDlgItem(hwndSettings, IDC_COMBO_CURSOR);
    int targetIndex = static_cast<int>(SendMessageW(hwndCombo, CB_GETCURSEL, 0, 0));
    
    if (targetIndex == CB_ERR) {
        MessageBoxW(hwndSettings, L"Please select a target cursor type first!", L"Error", MB_OK | MB_ICONWARNING);
        return;
    }

    CreateDirectoryW(CONFIG_DIR, nullptr);

    // Auto-copy dragged cursor files into the config folder with the correct name
    if (CopyFileW(sourcePath, cursorMap[targetIndex].filename, FALSE)) {
        ReloadCursorTexture(targetIndex);
        MessageBoxW(hwndSettings, L"Texture applied and updated on-the-fly successfully!", L"Success", MB_OK | MB_ICONINFORMATION);
    } else {
        DWORD errorId = GetLastError();
        wchar_t errorMsg[256];
        swprintf_s(errorMsg, L"Failed to copy file. Error Code: %lu", errorId);
        MessageBoxW(hwndSettings, errorMsg, L"I/O Error", MB_OK | MB_ICONERROR);
    }
}

// ============================================================================
// SETTINGS WINDOW CALLBACK
// ============================================================================

// Window procedure for the settings configuration dialog
// Handles UI controls for cursor selection and file import
LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            // Create UI controls for settings window
            // Label for cursor type selection
            CreateWindowExW(0, L"STATIC", L"1. Select Target Cursor Asset Type:", WS_CHILD | WS_VISIBLE, 
                            20, 20, 340, 20, hwnd, nullptr, nullptr, nullptr);

            // Dropdown menu to select which cursor type to customize
            HWND hwndCombo = CreateWindowExW(0, L"COMBOBOX", nullptr, 
                                            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 
                                            20, 45, 340, 200, hwnd, reinterpret_cast<HMENU>(IDC_COMBO_CURSOR), nullptr, nullptr);
            
            // Populate dropdown with cursor type names
            SendMessageW(hwndCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Default Arrow (pointer.png)"));
            SendMessageW(hwndCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Link Hand (link.png)"));
            SendMessageW(hwndCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Text I-Beam (text.png)"));
            SendMessageW(hwndCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Horizontal Scale (horz.png)"));
            SendMessageW(hwndCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Vertical Scale (vert.png)"));
            SendMessageW(hwndCombo, CB_SETCURSEL, 0, 0);

            // Label for file loading section
            CreateWindowExW(0, L"STATIC", L"2. Load Image File Configuration:", WS_CHILD | WS_VISIBLE, 
                            20, 95, 340, 20, hwnd, nullptr, nullptr, nullptr);

            // Button to browse and select PNG file from file system
            CreateWindowExW(0, L"BUTTON", L"Browse System PNG...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                            20, 120, 160, 30, hwnd, reinterpret_cast<HMENU>(IDC_BTN_BROWSE), nullptr, nullptr);

            // Helper text explaining drag-drop functionality
            CreateWindowExW(0, L"STATIC", L"💡 Interaction Tip:\nYou can drag and drop your custom .png image file directly anywhere onto this UI surface area.", 
                            WS_CHILD | WS_VISIBLE, 20, 170, 340, 50, hwnd, nullptr, nullptr, nullptr);

            // Enable drag-drop functionality for this window
            DragAcceptFiles(hwnd, TRUE); 
            return 0;
        }

        case WM_DROPFILES: {
            // User dragged and dropped a file onto the window
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            wchar_t droppedPath[MAX_PATH];
            
            if (DragQueryFileW(hDrop, 0, droppedPath, MAX_PATH)) {
                ProcessImportedFile(hwnd, droppedPath);
            }
            DragFinish(hDrop);
            return 0;
        }

        case WM_COMMAND: {
            // User clicked a button or changed dropdown selection
            if (LOWORD(wParam) == IDC_BTN_BROWSE) {
                // Open file browser dialog to select PNG image
                OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
                wchar_t selectedPath[MAX_PATH] = { 0 };
                
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = selectedPath;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrFilter = L"Portable Network Graphics (*.png)\0*.png\0";
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                
                if (GetOpenFileNameW(&ofn)) {
                    ProcessImportedFile(hwnd, selectedPath);
                }
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_hwndSettings = nullptr; 
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// ============================================================================
// MAIN WINDOW CALLBACK
// ============================================================================

// Window procedure for the invisible overlay window that handles:
// - System tray icon and menu
// - Rendering timer loop
// - Hotkey input (Ctrl+Alt+Q to exit)
LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_TRAYICON:
            // System tray icon right-click menu
            if (lParam == WM_RBUTTONUP) {
                POINT curPoint;
                GetCursorPos(&curPoint);
                
                // Build and display context menu
                HMENU hMenu = CreatePopupMenu();
                if (hMenu) {
                    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"Open Settings GUI...");
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                    // Check current mode to show visual indicator
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
            }
            return 0;

        case WM_TIMER:
            // Rendering loop timer tick
            // Note: Also called from main message loop when no messages are pending
            if (wParam == TIMER_RENDER_LOOP) {
                RenderCursor(hwnd); // Kept alive by internal modal message loops
            }
            return 0;

        case WM_COMMAND:
            // Handle menu selections and button clicks from tray menu or settings
            switch (LOWORD(wParam)) {
                case ID_TRAY_SETTINGS:
                    // Open settings dialog (or bring to front if already open)
                    if (!g_hwndSettings) {
                        g_hwndSettings = CreateWindowExW(
                            0, L"CursorSettingsClass", L"Cursor Overlay Settings Engine",
                            WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
                            CW_USEDEFAULT, CW_USEDEFAULT, 400, 270,
                            nullptr, nullptr, GetModuleHandle(nullptr), nullptr
                        );
                        ShowWindow(g_hwndSettings, SW_SHOW);
                    } else {
                        SetForegroundWindow(g_hwndSettings);
                    }
                    break;
                case ID_TRAY_EXIT:
                    // Close settings window if open, then exit
                    if (g_hwndSettings) DestroyWindow(g_hwndSettings);
                    DestroyWindow(hwnd);
                    break;
                case ID_TRAY_RELOAD:
                    // Reload physics settings from settings.ini file
                    LoadOrWriteConfig();
                    break;
                case ID_TRAY_MODE_STRETCH:
                    // Switch to STRETCH mode and save to settings
                    UpdateINIMode(MODE_STRETCH);
                    break;
                case ID_TRAY_MODE_ROTATE:
                    // Switch to ROTATE mode and save to settings
                    UpdateINIMode(MODE_ROTATE);
                    break;
            }
            return 0;

        case WM_HOTKEY:
            // Global hotkey handler (Ctrl+Alt+Q)
            if (wParam == 1) {
                if (g_hwndSettings) DestroyWindow(g_hwndSettings);
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_DESTROY:
            // Cleanup when window is being destroyed
            KillTimer(hwnd, TIMER_RENDER_LOOP);
            Shell_NotifyIconW(NIM_DELETE, &nid);  // Remove tray icon
            UnregisterHotKey(hwnd, 1);             // Unregister global hotkey
            RemovePropW(hwnd, L"NonRudeHWND");
            CleanUpD2D();                           // Release graphics resources
            PostQuitMessage(0);                     // Signal message loop to exit
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// ============================================================================
// PROGRAM ENTRY POINT
// ============================================================================

// Main Windows application entry point
// Sets up window classes, creates overlay window, and runs message loop
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Enable DPI awareness for proper scaling on high-DPI displays
    SetProcessDPIAware();
    // Load physics configuration from settings.ini (creates default if missing)
    LoadOrWriteConfig();
    // Set timer resolution to 1ms for more responsive rendering
    timeBeginPeriod(1);
    // Initialize COM for Windows Imaging Component (image loading)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ========== Register Settings Window Class ==========
    WNDCLASSW settingsWc = {};
    settingsWc.lpfnWndProc = SettingsWndProc;
    settingsWc.hInstance = hInstance;
    settingsWc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    settingsWc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    settingsWc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    settingsWc.lpszClassName = L"CursorSettingsClass";
    RegisterClassW(&settingsWc);

    const wchar_t CLASS_NAME[] = L"CursorOverlayClass";
    WNDCLASSW wc = {}; 
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc); 

    // ========== Get Virtual Screen Dimensions ==========
    // Create overlay window that spans all connected monitors
    int virtualLeft   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtualTop    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int virtualWidth  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // ========== Create Invisible Overlay Window ==========
    // WS_EX_LAYERED: Supports per-pixel alpha transparency
    // WS_EX_TRANSPARENT: Clicks pass through the window
    // WS_EX_TOPMOST: Stay above all windows
    // WS_EX_TOOLWINDOW: Hide from taskbar
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

    // Timer that runs the rendering loop synced to your monitor refresh rate
    SetTimer(hwnd, TIMER_RENDER_LOOP, 1, nullptr);

    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION); 
    wcscpy_s(nid.szTip, L"Dynamic Cursor Overlay");
    Shell_NotifyIconW(NIM_ADD, &nid);

    // Display the window (invisible but active)
    ShowWindow(hwnd, nCmdShow);

    // ========== Main Message Loop ==========
    // Processes Windows messages and keeps rendering loop running
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        // Check for pending messages (non-blocking)
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            // No messages pending: render the next frame
            // This keeps animation smooth during idle time
            RenderCursor(hwnd);
        }
    }

    // ========== Cleanup ==========
    CoUninitialize();       // Shutdown COM
    timeEndPeriod(1);       // Restore timer resolution
    return 0;
}