import re

with open('glazewm-taskbar-workspaces.wh.cpp', 'r', encoding='utf-8') as f:
    code = f.read()

# Replace globals
code = code.replace('static HWND                 g_widgetHwnd = nullptr;', 'static std::vector<HWND>    g_widgets;')
code = code.replace('static HWND                 g_taskbarHwnd = nullptr;\n', '')

# Replace PositionWidget
pos_old = r'''static void PositionWidget(int contentWidth) {
    if (!g_widgetHwnd || !g_taskbarHwnd) return;

    RECT taskbarRect;
    GetWindowRect(g_taskbarHwnd, &taskbarRect);

    int width = std::max(contentWidth, 60);
    int x;

    if (g_settings.positionRight) {
        // Find tray notification area
        HWND tray = FindWindowExW(g_taskbarHwnd, nullptr, L"TrayNotifyWnd", nullptr);
        if (!tray) {
            // Windows 11: try clock
            tray = FindWindowExW(g_taskbarHwnd, nullptr, L"TrayClockWClass", nullptr);
        }
        if (tray) {
            RECT trayRect;
            GetWindowRect(tray, &trayRect);
            x = trayRect.left - width - 4;
        } else {
            x = taskbarRect.right - width - 200;
        }
    } else {
        x = taskbarRect.left + 4;
    }

    MoveWindow(g_widgetHwnd, x, taskbarRect.top, width, g_settings.barHeight, TRUE);
}'''
pos_new = r'''static void PositionWidget(HWND hwnd, int contentWidth) {
    if (!hwnd) return;
    HWND taskbarHwnd = (HWND)GetWindowLongPtrW(hwnd, GWLP_HWNDPARENT);
    if (!taskbarHwnd) return;

    RECT taskbarRect;
    GetWindowRect(taskbarHwnd, &taskbarRect);

    int width = std::max(contentWidth, 60);
    int x;

    if (g_settings.positionRight) {
        HWND tray = FindWindowExW(taskbarHwnd, nullptr, L"TrayNotifyWnd", nullptr);
        if (!tray) {
            tray = FindWindowExW(taskbarHwnd, nullptr, L"TrayClockWClass", nullptr);
        }
        if (tray) {
            RECT trayRect;
            GetWindowRect(tray, &trayRect);
            x = trayRect.left - width - 4;
        } else {
            x = taskbarRect.right - width - 200;
        }
    } else {
        x = taskbarRect.left + 4;
    }

    MoveWindow(hwnd, x, taskbarRect.top, width, g_settings.barHeight, TRUE);
}'''
code = code.replace(pos_old, pos_new)

# In PaintWidget
code = code.replace('PositionWidget(totalWidth);', 'PositionWidget(hwnd, totalWidth);')

# Replace UIThreadProc
ui_old = r'''static void UIThreadProc() {
    Wh_Log(L"UI thread started");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WidgetWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = WIDGET_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            Wh_Log(L"RegisterClassEx failed: %d", err);
            return;
        }
    }

    g_taskbarHwnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!g_taskbarHwnd) {
        Wh_Log(L"Could not find taskbar window");
        return;
    }

    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED;
    g_widgetHwnd = CreateWindowExW(exStyle, WIDGET_CLASS, L"GlazeWM Workspaces",
                                    WS_POPUP, 0, 0, 300, g_settings.barHeight,
                                    nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!g_widgetHwnd) {
        Wh_Log(L"CreateWindowEx failed: %d", GetLastError());
        return;
    }

    if (g_settings.transparent) {
        SetLayeredWindowAttributes(g_widgetHwnd, TRANSPARENT_KEY_COLOR, 0, LWA_COLORKEY);
    } else {
        SetLayeredWindowAttributes(g_widgetHwnd, 0, 255, LWA_ALPHA);
    }

    PositionWidget(300);
    ShowWindow(g_widgetHwnd, SW_SHOWNOACTIVATE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_widgetHwnd = nullptr;
    UnregisterClassW(WIDGET_CLASS, GetModuleHandle(nullptr));
    Wh_Log(L"UI thread stopped");
}'''
ui_new = r'''static void UIThreadProc() {
    Wh_Log(L"UI thread started");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WidgetWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = WIDGET_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            Wh_Log(L"RegisterClassEx failed: %d", err);
            return;
        }
    }

    auto enumTaskbars = [](HWND hwnd, LPARAM lParam) -> BOOL {
        WCHAR className[256];
        GetClassNameW(hwnd, className, 256);
        if (wcscmp(className, L"Shell_TrayWnd") == 0 || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) {
            DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED;
            HWND widget = CreateWindowExW(exStyle, WIDGET_CLASS, L"GlazeWM Workspaces",
                                     WS_POPUP, 0, 0, 300, g_settings.barHeight,
                                     nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
            if (widget) {
                if (g_settings.transparent) {
                    SetLayeredWindowAttributes(widget, TRANSPARENT_KEY_COLOR, 0, LWA_COLORKEY);
                } else {
                    SetLayeredWindowAttributes(widget, 0, 255, LWA_ALPHA);
                }
                
                SetWindowLongPtrW(widget, GWLP_HWNDPARENT, (LONG_PTR)hwnd);
                PositionWidget(widget, 300);
                ShowWindow(widget, SW_SHOWNOACTIVATE);
                g_widgets.push_back(widget);
            }
        }
        return TRUE;
    };
    EnumWindows(enumTaskbars, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_widgets.clear();
    UnregisterClassW(WIDGET_CLASS, GetModuleHandle(nullptr));
    Wh_Log(L"UI thread stopped");
}'''
code = code.replace(ui_old, ui_new)

# Broadcasts
code = code.replace('if (g_widgetHwnd) PostMessage(g_widgetHwnd, WM_GLAZE_REFRESH, 0, 0);', 'for (auto w : g_widgets) PostMessage(w, WM_GLAZE_REFRESH, 0, 0);')
code = code.replace('if (g_widgetHwnd)\n                    PostMessage(g_widgetHwnd, WM_GLAZE_EVENT, immediate ? 1 : 0, 0);', 'for (auto w : g_widgets)\n                    PostMessage(w, WM_GLAZE_EVENT, immediate ? 1 : 0, 0);')
code = code.replace('while (g_running.load() && !g_widgetHwnd) {', 'while (g_running.load() && g_widgets.empty()) {')
code = code.replace('if (g_widgetHwnd) {\n        PostMessage(g_widgetHwnd, WM_CLOSE, 0, 0);\n    }', 'for (auto w : g_widgets) PostMessage(w, WM_CLOSE, 0, 0);')
code = code.replace('if (g_widgetHwnd) {\n        if (g_settings.transparent) {', 'for (auto w : g_widgets) {\n        if (g_settings.transparent) {')
code = code.replace('InvalidateRect(g_widgetHwnd, nullptr, TRUE);\n    }', 'InvalidateRect(w, nullptr, TRUE);\n    }')
code = code.replace('if (QueryAndUpdateState() && g_widgetHwnd)\n                PostMessage(g_widgetHwnd, WM_GLAZE_REFRESH, 0, 0);', 'if (QueryAndUpdateState()) {\n                for (auto w : g_widgets) PostMessage(w, WM_GLAZE_REFRESH, 0, 0);\n            }')
code = code.replace('if (QueryAndUpdateState())\n                    PostMessage(hwnd, WM_GLAZE_REFRESH, 0, 0);', 'if (QueryAndUpdateState())\n                    for (auto w : g_widgets) PostMessage(w, WM_GLAZE_REFRESH, 0, 0);')

with open('glazewm-taskbar-workspaces.wh.cpp', 'w', encoding='utf-8') as f:
    f.write(code)

print('Refactoring complete')