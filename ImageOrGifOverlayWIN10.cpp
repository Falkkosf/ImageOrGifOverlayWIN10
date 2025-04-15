#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <cmath>
#include <chrono>
#include <thread>
#include <string>
#include <atomic>
#include <mutex>
#include <algorithm>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

struct AppState {
    HINSTANCE hInstance;
    ULONG_PTR gdiplusToken;
    Bitmap* image = nullptr;
    HWND hwnd;
    HMENU hMenu;

    // For animated GIFs
    GUID* dimensionIDs = nullptr;
    UINT frameCount = 0;
    UINT currentFrame = 0;
    bool isAnimatedGIF = false;
    PropertyItem* frameDelays = nullptr;
    std::thread animationThread;
    std::atomic<bool> stopAnimation{ false };

    // Transformation
    float scale = 1.0f;
    float rotation = 0.0f;
    float moveSpeed = 1.0f;

    // Cursor tracking
    POINT cursorPos{ 0, 0 };
    POINT grabPointImage{ 0, 0 };
    std::atomic<bool> isDragging{ false };
    std::atomic<bool> wasMoved{ false };

    // Performance optimization
    HBITMAP hCachedBitmap = nullptr;
    SIZE cachedSize{ 0, 0 };
    float cachedScale = 0.0f;
    float cachedRotation = 0.0f;
    std::mutex renderMutex;
};

SIZE CalculateRotatedSize(int width, int height, float angleDeg, float scale);
PointF TransformPoint(const POINT& pt, float angleDeg, float scale, int imgWidth, int imgHeight);
void UpdateLayeredWindowContent(HWND hwnd, AppState* state, bool forceUpdate = false);
void AdjustWindowPosition(AppState* state);
bool LoadNewImage(AppState* state, const wchar_t* filePath);

void ShowOpenImageDialog(AppState* state);
void ShowControlsTooltip(HWND hwnd);
void AnimateGIF(AppState* state);
void CleanupCachedBitmap(AppState* state);
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

SIZE CalculateRotatedSize(int width, int height, float angleDeg, float scale) {
    float radians = angleDeg * 3.14159265f / 180.0f;
    float cosA = std::abs(std::cos(radians));
    float sinA = std::abs(std::sin(radians));

    int scaledWidth = static_cast<int>(width * scale);
    int scaledHeight = static_cast<int>(height * scale);

    SIZE result;
    result.cx = static_cast<int>(scaledWidth * cosA + scaledHeight * sinA);
    result.cy = static_cast<int>(scaledWidth * sinA + scaledHeight * cosA);
    return result;
}

PointF TransformPoint(const POINT& pt, float angleDeg, float scale, int imgWidth, int imgHeight) {
    float centerX = imgWidth / 2.0f;
    float centerY = imgHeight / 2.0f;

    float x = pt.x - centerX;
    float y = pt.y - centerY;

    float radians = angleDeg * 3.14159265f / 180.0f;
    float cosA = std::cos(radians);
    float sinA = std::sin(radians);

    return PointF(
        x * scale * cosA - y * scale * sinA,
        x * scale * sinA + y * scale * cosA
    );
}

void UpdateLayeredWindowContent(HWND hwnd, AppState* state, bool forceUpdate) {
    if (!state || !state->image) return;

    std::lock_guard<std::mutex> lock(state->renderMutex);

    int imgWidth = state->image->GetWidth();
    int imgHeight = state->image->GetHeight();

    SIZE canvasSize = CalculateRotatedSize(imgWidth, imgHeight, state->rotation, state->scale);

    if (state->hCachedBitmap && !forceUpdate &&
        state->cachedScale == state->scale &&
        state->cachedRotation == state->rotation &&
        state->cachedSize.cx == canvasSize.cx &&
        state->cachedSize.cy == canvasSize.cy) {
        // Use cached bitmap
    }
    else {
        CleanupCachedBitmap(state);

        Bitmap canvas(canvasSize.cx, canvasSize.cy, PixelFormat32bppARGB);
        Graphics graphics(&canvas);

        graphics.SetSmoothingMode(SmoothingModeHighQuality);
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.Clear(Color(0, 0, 0, 0));

        graphics.TranslateTransform(canvasSize.cx / 2.0f, canvasSize.cy / 2.0f);
        graphics.RotateTransform(state->rotation);
        graphics.ScaleTransform(state->scale, state->scale);
        graphics.TranslateTransform(-imgWidth / 2.0f, -imgHeight / 2.0f);

        graphics.DrawImage(state->image, 0, 0, imgWidth, imgHeight);

        canvas.GetHBITMAP(Color(0, 0, 0, 0), &state->hCachedBitmap);
        state->cachedScale = state->scale;
        state->cachedRotation = state->rotation;
        state->cachedSize = canvasSize;
    }

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HGDIOBJ oldBitmap = SelectObject(memDC, state->hCachedBitmap);

    POINT srcPoint = { 0, 0 };
    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);
    POINT dstPoint = { windowRect.left, windowRect.top };

    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(
        hwnd, screenDC, &dstPoint, &canvasSize,
        memDC, &srcPoint, 0, &blend, ULW_ALPHA
    );

    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

void AdjustWindowPosition(AppState* state) {
    if (!state || !state->isDragging || !state->image) return;

    GetCursorPos(&state->cursorPos);

    int imgWidth = state->image->GetWidth();
    int imgHeight = state->image->GetHeight();

    PointF transformed = TransformPoint(
        state->grabPointImage,
        state->rotation,
        state->scale,
        imgWidth,
        imgHeight
    );

    SIZE rotatedSize = CalculateRotatedSize(imgWidth, imgHeight, state->rotation, state->scale);

    int newX = state->cursorPos.x - static_cast<int>((rotatedSize.cx / 2.0f + transformed.X) * state->moveSpeed);
    int newY = state->cursorPos.y - static_cast<int>((rotatedSize.cy / 2.0f + transformed.Y) * state->moveSpeed);

    SetWindowPos(
        state->hwnd,
        nullptr,
        newX,
        newY,
        rotatedSize.cx,
        rotatedSize.cy,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
    );
}

void CleanupCachedBitmap(AppState* state) {
    if (state && state->hCachedBitmap) {
        DeleteObject(state->hCachedBitmap);
        state->hCachedBitmap = nullptr;
    }
}

bool LoadNewImage(AppState* state, const wchar_t* filePath) {
    if (!state) return false;

    // Stop and clean previous animation if exists
    state->stopAnimation = true;
    if (state->animationThread.joinable()) {
        state->animationThread.join();
    }

    // Cleanup previous resources
    delete state->image;
    state->image = nullptr;
    CleanupCachedBitmap(state);

    if (state->dimensionIDs) {
        delete[] state->dimensionIDs;
        state->dimensionIDs = nullptr;
    }

    if (state->frameDelays) {
        delete[] state->frameDelays;
        state->frameDelays = nullptr;
    }

    // Load new image
    IStream* pStream = nullptr;
    if (SHCreateStreamOnFileEx(filePath, STGM_READ, 0, FALSE, nullptr, &pStream) != S_OK) {
        return false;
    }

    Bitmap* newImage = Bitmap::FromStream(pStream);
    pStream->Release();

    if (!newImage || newImage->GetLastStatus() != Ok) {
        delete newImage;
        return false;
    }

    // Check if it's an animated GIF
    state->isAnimatedGIF = false;
    state->frameCount = 0;
    state->currentFrame = 0;

    UINT count = newImage->GetFrameDimensionsCount();
    if (count > 0) {
        state->dimensionIDs = new GUID[count];
        newImage->GetFrameDimensionsList(state->dimensionIDs, count);

        state->frameCount = newImage->GetFrameCount(&state->dimensionIDs[0]);
        if (state->frameCount > 1) {
            state->isAnimatedGIF = true;

            // Get frame delays
            UINT size = newImage->GetPropertyItemSize(PropertyTagFrameDelay);
            if (size > 0) {
                state->frameDelays = (PropertyItem*)new char[size];
                if (newImage->GetPropertyItem(PropertyTagFrameDelay, size, state->frameDelays) == Ok) {
                    // Start animation thread
                    state->stopAnimation = false;
                    state->animationThread = std::thread([state]() {
                        while (!state->stopAnimation.load()) {
                            {
                                std::lock_guard<std::mutex> lock(state->renderMutex);
                                if (state->image) {
                                    state->image->SelectActiveFrame(&state->dimensionIDs[0], state->currentFrame);
                                }
                            }

                            // Force redraw
                            UpdateLayeredWindowContent(state->hwnd, state, true);

                            // Get frame delay (in 10ms units)
                            long delay = ((long*)state->frameDelays->value)[state->currentFrame] * 10;
                            delay = std::max(10L, std::min(delay, 1000L)); // Clamp between 10ms and 1000ms

                            std::this_thread::sleep_for(std::chrono::milliseconds(delay));

                            // Next frame
                            state->currentFrame = (state->currentFrame + 1) % state->frameCount;
                        }
                        });
                }
                else {
                    delete[] state->frameDelays;
                    state->frameDelays = nullptr;
                    state->isAnimatedGIF = false;
                }
            }
        }
    }

    state->image = newImage;
    UpdateLayeredWindowContent(state->hwnd, state, true);
    return true;
}


void AnimateGIF(AppState* state) {
    if (!state || !state->isAnimatedGIF) return;

    while (!state->stopAnimation.load()) {
        {
            std::lock_guard<std::mutex> lock(state->renderMutex);
            if (state->image) {
                state->image->SelectActiveFrame(&state->dimensionIDs[0], state->currentFrame);
            }
        }

        // Force immediate redraw
        UpdateLayeredWindowContent(state->hwnd, state);

        // Calculate delay with original speed
        long delay = ((long*)state->frameDelays->value)[state->currentFrame] * 10;
        delay = std::max(10L, std::min(delay, 1000L)); // Clamp between 10ms and 1000ms

        std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        // Move to next frame
        state->currentFrame = (state->currentFrame + 1) % state->frameCount;
    }
}

void ShowOpenImageDialog(AppState* state) {
    if (!state) return;

    OPENFILENAME ofn;
    wchar_t fileName[MAX_PATH] = L"";

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = state->hwnd;
    ofn.lpstrFilter = L"All Supported Images\0*.bmp;*.jpg;*.jpeg;*.png;*.gif;*.tif;*.tiff;*.ico\0"
        L"Bitmap Images (*.bmp)\0*.bmp\0"
        L"JPEG Images (*.jpg, *.jpeg)\0*.jpg;*.jpeg\0"
        L"PNG Images (*.png)\0*.png\0"
        L"GIF Images (*.gif)\0*.gif\0"
        L"TIFF Images (*.tif, *.tiff)\0*.tif;*.tiff\0"
        L"Icon Files (*.ico)\0*.ico\0"
        L"All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrDefExt = L"png";
    ofn.lpstrTitle = L"Select Image File";

    if (GetOpenFileName(&ofn)) {
        if (!LoadNewImage(state, fileName)) {
            MessageBox(state->hwnd, L"Failed to load image", L"Error", MB_ICONERROR);
        }
        else {
            ShowControlsTooltip(state->hwnd);
        }
    }
}

void ShowControlsTooltip(HWND hwnd) {
    const wchar_t* controlsText =
        L"Controls:\n"
        L"Right Mouse Button - Drag image\n"
        L"Mouse Wheel - Zoom (hold Ctrl for fine control)\n"
        L"Alt + Mouse Wheel - Rotate\n"
        L"Shift + Mouse Wheel - Adjust movement speed\n"
        L"Esc - Close application\n"
        L"File Menu - Open new image";

    MessageBox(hwnd, controlsText, L"Image Overlay Controls", MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT* create = reinterpret_cast<CREATESTRUCT*>(lParam);
        state = new AppState();
        state->hInstance = create->hInstance;
        state->hwnd = hwnd;

        state->hMenu = CreateMenu();
        HMENU hFileMenu = CreatePopupMenu();
        AppendMenu(hFileMenu, MF_STRING, 1001, L"&Open Image...");
        AppendMenu(hFileMenu, MF_STRING, 1002, L"&Controls Help");
        AppendMenu(hFileMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hFileMenu, MF_STRING, 1003, L"E&xit");
        AppendMenu(state->hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hFileMenu), L"&File");
        SetMenu(hwnd, state->hMenu);

        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        // Try to load default image
        if (!LoadNewImage(state, L"image.png") &&
            !LoadNewImage(state, L"image.jpg") &&
            !LoadNewImage(state, L"image.gif")) {
            // If no default image found, show open dialog
            ShowOpenImageDialog(state);
        }
        else {
            // Show controls only if image loaded successfully
            ShowControlsTooltip(hwnd);
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1001: // Open Image
            ShowOpenImageDialog(state);
            break;
        case 1002: // Controls Help
            ShowControlsTooltip(hwnd);
            break;
        case 1003: // Exit
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        }
        break;

    case WM_RBUTTONDOWN: {
        if (!state) break;

        state->isDragging = true;
        state->wasMoved = false;
        SetCapture(hwnd);

        GetCursorPos(&state->cursorPos);

        RECT windowRect;
        GetWindowRect(hwnd, &windowRect);

        int localX = state->cursorPos.x - windowRect.left;
        int localY = state->cursorPos.y - windowRect.top;

        SIZE rotated = CalculateRotatedSize(
            state->image->GetWidth(),
            state->image->GetHeight(),
            state->rotation,
            state->scale
        );

        float centerX = rotated.cx / 2.0f;
        float centerY = rotated.cy / 2.0f;

        float x = localX - centerX;
        float y = localY - centerY;

        float radians = -state->rotation * 3.14159265f / 180.0f;
        float cosA = std::cos(radians);
        float sinA = std::sin(radians);

        float rx = (x * cosA - y * sinA) / state->scale;
        float ry = (x * sinA + y * cosA) / state->scale;

        state->grabPointImage.x = static_cast<int>(rx + state->image->GetWidth() / 2.0f);
        state->grabPointImage.y = static_cast<int>(ry + state->image->GetHeight() / 2.0f);

        AdjustWindowPosition(state);
        break;
    }

    case WM_RBUTTONUP:
        if (state) {
            state->isDragging = false;
            state->wasMoved = false;
            ReleaseCapture();
        }
        break;

    case WM_MOUSEMOVE:
        if (state && state->isDragging) {
            state->wasMoved = true;
            AdjustWindowPosition(state);
        }
        break;

    case WM_MOUSEWHEEL: {
        if (!state) break;

        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        bool ctrlPressed = GetKeyState(VK_CONTROL) & 0x8000;
        bool altPressed = GetKeyState(VK_MENU) & 0x8000;
        bool shiftPressed = GetKeyState(VK_SHIFT) & 0x8000;

        if (shiftPressed) {
            state->moveSpeed += (delta > 0) ? 0.1f : -0.1f;
            state->moveSpeed = std::max(0.1f, std::min(state->moveSpeed, 5.0f));
        }
        else if (ctrlPressed) {
            state->scale += (delta > 0) ? 0.01f : -0.01f;
            state->scale = std::max(0.05f, std::min(state->scale, 10.0f));
        }
        else if (altPressed) {
            state->rotation += (delta > 0) ? 2.0f : -2.0f;
            state->rotation = fmod(state->rotation, 360.0f);
        }
        else {
            state->scale += (delta > 0) ? 0.05f : -0.05f;
            state->scale = std::max(0.05f, std::min(state->scale, 10.0f));
        }

        AdjustWindowPosition(state);
        UpdateLayeredWindowContent(hwnd, state);
        break;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        break;

    case WM_DESTROY:
        if (state) {
            state->stopAnimation = true;
            if (state->animationThread.joinable()) {
                state->animationThread.join();
            }
            delete state->image;
            CleanupCachedBitmap(state);
            if (state->dimensionIDs) delete[] state->dimensionIDs;
            if (state->frameDelays) delete[] state->frameDelays;
            DestroyMenu(state->hMenu);
            delete state;
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ImageOverlayWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClass(&wc)) {
        MessageBox(nullptr, L"Window registration failed", L"Error", MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        L"Image Overlay",
        WS_POPUP | WS_VISIBLE | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        800, 600,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        MessageBox(nullptr, L"Window creation failed", L"Error", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    return static_cast<int>(msg.wParam);
}