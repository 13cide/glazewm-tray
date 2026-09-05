// ==WindhawkMod==
// @id              glazewm-taskbar-workspaces
// @name            GlazeWM Taskbar Workspaces
// @description     Native GlazeWM workspace indicators on the Windows taskbar with app icons and click-to-switch
// @version         1.0.0
// @author          13cide
// @github          https://github.com/13cide
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lwinhttp -lshell32 -lgdi32 -lshlwapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# GlazeWM Taskbar Workspaces

A native taskbar widget for the [GlazeWM](https://github.com/glzr-io/glazewm)
tiling window manager. Displays workspace indicators with real application
icons directly on the Windows taskbar.

## Features
- **Workspace indicators** with active highlight and occupancy markers
- **Real app icons** extracted from running processes
- **Click to switch** workspace or focus/minimize individual windows
- **Auto-toggle tiling** direction on new windows
- **Fullscreen aware** — auto-hides during fullscreen apps
- **Transparent or dark** background modes
- **Right-click menu** for GlazeWM commands
- **Zero CPU when idle** — event-driven via WebSocket

## Requirements
- [GlazeWM](https://github.com/glzr-io/glazewm) running with WebSocket
  server on `ws://127.0.0.1:6123` (default)
- Windows 10 or 11
- [Windhawk](https://windhawk.net/)

## Usage
1. Install this mod in Windhawk
2. Make sure GlazeWM is running
3. The workspace bar appears on your taskbar automatically
4. Configure colors, position, and behavior in the mod settings
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- wsUrl: ws://127.0.0.1:6123
  $name: GlazeWM WebSocket URL
  $description: WebSocket endpoint for GlazeWM (default ws://127.0.0.1:6123)
- autoToggleTiling: true
  $name: Auto-Toggle Tiling
  $description: Automatically toggle tiling direction when a new window opens
- transparent: true
  $name: Transparent Background
  $description: Use transparent background (shows taskbar through). Disable for opaque dark background.
- iconsOnly: false
  $name: Icons Only
  $description: Show only app icons, hide window title text (compact mode)
- positionRight: true
  $name: Position Right
  $description: Place bar on the right side of the taskbar (near system tray). Disable for left side.
- labelLeft: true
  $name: Label Before Icons
  $description: Show workspace number before its window icons. Disable to show after.
- workspaceGap: 3
  $name: Workspace Gap (px)
  $description: Pixel gap between workspace sections (3 = compact, 12 = wide)
- debounceMs: 300
  $name: Debounce Delay (ms)
  $description: Milliseconds to wait after burst events (window open/close) before refreshing
- bgColor: "#141414"
  $name: Background Color
  $description: Background color when not transparent (hex RGB)
- textColor: "#FFFFFF"
  $name: Text Color
  $description: Color for workspace numbers and window titles
- activeColor: "#42C0FB"
  $name: Active Workspace Color
  $description: Highlight color for the focused workspace
- inactiveColor: "#646464"
  $name: Inactive Color
  $description: Color for empty/unfocused workspace numbers
- errorColor: "#FF6464"
  $name: Error Color
  $description: Color for error indicators
*/
// ==/WindhawkModSettings==

// =====================================================================
// INCLUDES
// =====================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <cstdlib>

// =====================================================================
// WINHTTP WEBSOCKET COMPATIBILITY (dynamic loading for MinGW)
// =====================================================================

#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif

// WebSocket buffer types
#define WS_BUF_BINARY_MSG    0
#define WS_BUF_BINARY_FRAG   1
#define WS_BUF_UTF8_MSG      2
#define WS_BUF_UTF8_FRAG     3
#define WS_BUF_CLOSE         4

typedef HINTERNET (WINAPI *pfn_WebSocketCompleteUpgrade)(HINTERNET, DWORD_PTR);
typedef DWORD     (WINAPI *pfn_WebSocketSend)(HINTERNET, int, PVOID, DWORD);
typedef DWORD     (WINAPI *pfn_WebSocketReceive)(HINTERNET, PVOID, DWORD, DWORD*, int*);
typedef DWORD     (WINAPI *pfn_WebSocketClose)(HINTERNET, USHORT, PVOID, DWORD);

static pfn_WebSocketCompleteUpgrade pWsCompleteUpgrade = nullptr;
static pfn_WebSocketSend            pWsSend            = nullptr;
static pfn_WebSocketReceive         pWsReceive         = nullptr;
static pfn_WebSocketClose           pWsClose           = nullptr;

static bool LoadWinHttpWebSocket() {
    HMODULE h = GetModuleHandleW(L"winhttp.dll");
    if (!h) h = LoadLibraryW(L"winhttp.dll");
    if (!h) return false;
    pWsCompleteUpgrade = (pfn_WebSocketCompleteUpgrade)GetProcAddress(h, "WinHttpWebSocketCompleteUpgrade");
    pWsSend            = (pfn_WebSocketSend)GetProcAddress(h, "WinHttpWebSocketSend");
    pWsReceive         = (pfn_WebSocketReceive)GetProcAddress(h, "WinHttpWebSocketReceive");
    pWsClose           = (pfn_WebSocketClose)GetProcAddress(h, "WinHttpWebSocketClose");
    return pWsCompleteUpgrade && pWsSend && pWsReceive && pWsClose;
}

// =====================================================================
// CONSTANTS
// =====================================================================

#define WIDGET_CLASS L"GlazeWM_Workspaces_Widget"
#define WM_GLAZE_EVENT   (WM_USER + 100)
#define WM_GLAZE_REFRESH (WM_USER + 101)
#define TIMER_DEBOUNCE   1
#define TIMER_FULLSCREEN 2
#define TIMER_RECONNECT  3
// Magic color for transparency (Near-black works best to avoid bright halos without making pure black transparent)
#define TRANSPARENT_KEY_COLOR RGB(1, 1, 1)

#define ID_MENU_BASE     5000
#define ID_WS_BASE       6000
#define ID_TOGGLE_FLOAT  5001
#define ID_TOGGLE_TILE   5002
#define ID_CLOSE_WIN     5003
#define ID_AUTO_TOGGLE   5004
#define ID_REDRAW        5005
#define ID_RELOAD        5006

// GlazeWM event types
static const char* IMMEDIATE_EVENTS[] = {
    "focus_changed", "workspace_activated", "workspace_deactivated",
    "workspace_updated", "focused_container_moved",
    "tiling_direction_changed", "binding_modes_changed", "pause_changed"
};

static const char* SUBSCRIBE_EVENTS[] = {
    "focus_changed", "workspace_activated", "workspace_deactivated",
    "workspace_updated", "window_managed", "window_unmanaged",
    "tiling_direction_changed", "binding_modes_changed",
    "focused_container_moved", "pause_changed"
};

// =====================================================================
// TYPES
// =====================================================================

struct WindowInfo {
    std::string id;
    int handle = 0;
    bool hasFocus = false;
    std::string title;
    std::string process;
    std::string state; // tiling, floating, minimized
};

struct WorkspaceInfo {
    std::string name;
    bool focused = false;
    bool hasWindows = false;
    std::vector<WindowInfo> windows;
};

struct MonitorInfo {
    std::string id;
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
    std::vector<WorkspaceInfo> workspaces;
};

struct HitRect {
    HWND hwnd;
    RECT rect;
    enum Type { WORKSPACE, WINDOW } type;
    std::string target;       // workspace name or window ID
    std::string wsName;       // workspace name (for window hits)
    bool windowFocused;
    std::string windowState;
};

struct Settings {
    std::wstring wsUrl = L"ws://127.0.0.1:6123";
    std::string  wsHost = "127.0.0.1";
    int          wsPort = 6123;
    bool  autoToggle   = true;
    bool  transparent  = true;
    bool  iconsOnly    = false;
    bool  positionRight = true;
    bool  labelLeft    = true;
    int   workspaceGap = 3;
    int   debounceMs   = 300;
    
    COLORREF bgColor      = RGB(20, 20, 20);
    COLORREF textColor    = RGB(255, 255, 255);
    COLORREF activeColor  = RGB(66, 192, 251);
    COLORREF inactiveColor = RGB(100, 100, 100);
    COLORREF errorColor   = RGB(255, 100, 100);
};

// =====================================================================
// UTILITIES
// =====================================================================

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}

static std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), s.data(), len, nullptr, nullptr);
    return s;
}

static COLORREF HexToColor(const char* hex) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return RGB(20, 20, 20);
    auto h = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    int r = h(hex[1]) * 16 + h(hex[2]);
    int g = h(hex[3]) * 16 + h(hex[4]);
    int b = h(hex[5]) * 16 + h(hex[6]);
    return RGB(r, g, b);
}

static bool IsImmediateEvent(const std::string& eventType) {
    for (auto& e : IMMEDIATE_EVENTS)
        if (eventType == e) return true;
    return false;
}

// =====================================================================
// MINIMAL JSON PARSER
// =====================================================================

class Json {
public:
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
    Type type = NUL;
    bool bval = false;
    double nval = 0;
    std::string sval;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    bool boolean(bool d = false) const { return type == BOOL ? bval : d; }
    double number(double d = 0) const { return type == NUM ? nval : d; }
    int integer(int d = 0) const { return type == NUM ? (int)nval : d; }
    const std::string& str() const { static std::string e; return type == STR ? sval : e; }
    size_t size() const { return type == ARR ? arr.size() : (type == OBJ ? obj.size() : 0); }

    const Json& operator[](const char* key) const {
        if (type == OBJ)
            for (auto& [k, v] : obj)
                if (k == key) return v;
        static Json null;
        return null;
    }

    const Json& operator[](size_t i) const {
        if (type == ARR && i < arr.size()) return arr[i];
        static Json null;
        return null;
    }

    bool has(const char* key) const {
        if (type == OBJ)
            for (auto& [k, v] : obj)
                if (k == key) return true;
        return false;
    }

    const std::vector<Json>& array() const { static std::vector<Json> e; return type == ARR ? arr : e; }

    static Json parse(const std::string& s) {
        const char* p = s.c_str();
        return parseValue(p);
    }

private:
    static void skipWs(const char*& p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }

    static std::string parseStr(const char*& p) {
        if (*p != '"') return {};
        p++; // skip opening quote
        std::string r;
        while (*p && *p != '"') {
            if (*p == '\\') {
                p++;
                switch (*p) {
                    case '"': r += '"'; break;
                    case '\\': r += '\\'; break;
                    case '/': r += '/'; break;
                    case 'n': r += '\n'; break;
                    case 'r': r += '\r'; break;
                    case 't': r += '\t'; break;
                    case 'b': r += '\b'; break;
                    case 'f': r += '\f'; break;
                    case 'u': {
                        // Basic \uXXXX - just skip for now, replace with ?
                        if (p[1] && p[2] && p[3] && p[4]) { p += 4; r += '?'; }
                        break;
                    }
                    default: r += *p;
                }
            } else {
                r += *p;
            }
            p++;
        }
        if (*p == '"') p++; // skip closing quote
        return r;
    }

    static Json parseValue(const char*& p) {
        skipWs(p);
        Json v;
        if (*p == '{') {
            v.type = OBJ;
            p++; // {
            skipWs(p);
            while (*p && *p != '}') {
                skipWs(p);
                std::string key = parseStr(p);
                skipWs(p);
                if (*p == ':') p++;
                Json val = parseValue(p);
                v.obj.push_back({key, std::move(val)});
                skipWs(p);
                if (*p == ',') p++;
            }
            if (*p == '}') p++;
        } else if (*p == '[') {
            v.type = ARR;
            p++; // [
            skipWs(p);
            while (*p && *p != ']') {
                v.arr.push_back(parseValue(p));
                skipWs(p);
                if (*p == ',') p++;
            }
            if (*p == ']') p++;
        } else if (*p == '"') {
            v.type = STR;
            v.sval = parseStr(p);
        } else if (*p == 't') {
            v.type = BOOL; v.bval = true;
            if (strncmp(p, "true", 4) == 0) p += 4;
        } else if (*p == 'f') {
            v.type = BOOL; v.bval = false;
            if (strncmp(p, "false", 5) == 0) p += 5;
        } else if (*p == 'n') {
            v.type = NUL;
            if (strncmp(p, "null", 4) == 0) p += 4;
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            v.type = NUM;
            char* end = nullptr;
            v.nval = strtod(p, &end);
            if (end) p = end;
        }
        return v;
    }
};

// =====================================================================
// WEBSOCKET CLIENT (WinHTTP)
// =====================================================================

class WsClient {
    HINTERNET m_session = nullptr;
    HINTERNET m_connect = nullptr;
    HINTERNET m_request = nullptr;
    HINTERNET m_ws = nullptr;

public:
    ~WsClient() { close(); }

    bool isOpen() const { return m_ws != nullptr; }

    bool connect(const std::wstring& host, int port) {
        close();
        m_session = WinHttpOpen(L"GlazeWM-Windhawk/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
        if (!m_session) return false;

        // Set timeouts: resolve, connect, send, receive (ms)
        DWORD timeout = 5000;
        WinHttpSetOption(m_session, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));

        m_connect = WinHttpConnect(m_session, host.c_str(), (INTERNET_PORT)port, 0);
        if (!m_connect) { close(); return false; }

        m_request = WinHttpOpenRequest(m_connect, L"GET", L"/", nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!m_request) { close(); return false; }

        // Request WebSocket upgrade
        BOOL optVal = TRUE;
        if (!WinHttpSetOption(m_request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
            close(); return false;
        }

        if (!WinHttpSendRequest(m_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) {
            close(); return false;
        }

        if (!WinHttpReceiveResponse(m_request, nullptr)) {
            close(); return false;
        }

        m_ws = pWsCompleteUpgrade(m_request, 0);
        if (!m_ws) { close(); return false; }

        // Close the request handle — no longer needed after upgrade
        WinHttpCloseHandle(m_request);
        m_request = nullptr;

        return true;
    }

    bool send(const std::string& msg) {
        if (!m_ws) return false;
        DWORD err = pWsSend(m_ws, WS_BUF_UTF8_MSG, (PVOID)msg.c_str(), (DWORD)msg.size());
        return err == 0;
    }

    std::string receive() {
        if (!m_ws) return {};
        std::string result;
        char buf[8192];
        while (true) {
            DWORD bytesRead = 0;
            int bufType = 0;
            DWORD err = pWsReceive(m_ws, buf, sizeof(buf), &bytesRead, &bufType);
            if (err != 0) return {};
            result.append(buf, bytesRead);
            if (bufType == WS_BUF_UTF8_MSG || bufType == WS_BUF_BINARY_MSG) break;
            if (bufType == WS_BUF_CLOSE) return {};
            // Fragment — continue receiving
        }
        return result;
    }

    void close() {
        if (m_ws) { pWsClose(m_ws, 1000, nullptr, 0); WinHttpCloseHandle(m_ws); m_ws = nullptr; }
        if (m_request) { WinHttpCloseHandle(m_request); m_request = nullptr; }
        if (m_connect) { WinHttpCloseHandle(m_connect); m_connect = nullptr; }
        if (m_session) { WinHttpCloseHandle(m_session); m_session = nullptr; }
    }

    // Force-close to unblock a blocking receive on another thread
    void abort() {
        if (m_ws) { WinHttpCloseHandle(m_ws); m_ws = nullptr; }
    }
};

// =====================================================================
// GLAZEWM STATE PARSER
// =====================================================================

static void CollectWindows(const Json& node, std::vector<WindowInfo>& out) {
    if (node.type == Json::OBJ) {
        if (node.has("type") && node["type"].str() == "window") {
            WindowInfo w;
            w.id = node["id"].str();
            w.handle = node["handle"].integer();
            w.hasFocus = node["hasFocus"].boolean();
            w.title = node["title"].str();
            w.process = node["processName"].str();
            // state can be { "type": "tiling" } or { "type": "minimized" } etc.
            if (node["state"].type == Json::OBJ)
                w.state = node["state"]["type"].str();
            else
                w.state = "tiling";
            out.push_back(std::move(w));
            return;
        }
        for (auto& [k, v] : node.obj)
            if (v.type == Json::OBJ || v.type == Json::ARR)
                CollectWindows(v, out);
    } else if (node.type == Json::ARR) {
        for (auto& el : node.arr)
            CollectWindows(el, out);
    }
}

static bool ParseMonitors(const Json& response, std::vector<MonitorInfo>& monitors,
                           std::string& currentWs, int& windowCount) {
    if (!response["success"].boolean()) return false;
    const Json& data = response["data"];

    // Find monitors array
    const Json* monArr = nullptr;
    if (data.has("monitors")) monArr = &data["monitors"];
    else if (data.type == Json::ARR) monArr = &data;
    else return false;

    monitors.clear();
    windowCount = 0;
    currentWs = "?";

    for (size_t mi = 0; mi < monArr->size(); mi++) {
        const Json& mNode = (*monArr)[mi];
        if (mNode["type"].str() != "monitor") continue;

        MonitorInfo mon;
        mon.id = mNode["id"].str();
        mon.name = mNode["name"].str();
        mon.x = mNode["x"].integer();
        mon.y = mNode["y"].integer();
        mon.width = mNode["width"].integer();
        mon.height = mNode["height"].integer();

        // Find workspaces in the monitor tree
        std::vector<const Json*> stack;
        stack.push_back(&mNode);
        while (!stack.empty()) {
            const Json* cur = stack.back(); stack.pop_back();
            if (cur->type == Json::OBJ) {
                if (cur->has("type") && (*cur)["type"].str() == "workspace") {
                    WorkspaceInfo ws;
                    ws.name = (*cur)["name"].str();
                    ws.focused = (*cur)["hasFocus"].boolean();
                    CollectWindows((*cur)["children"], ws.windows);
                    ws.hasWindows = !ws.windows.empty();
                    windowCount += (int)ws.windows.size();
                    if (ws.focused) currentWs = ws.name;
                    mon.workspaces.push_back(std::move(ws));
                } else {
                    for (auto& [k, v] : cur->obj)
                        if (v.type == Json::OBJ || v.type == Json::ARR)
                            stack.push_back(&v);
                }
            } else if (cur->type == Json::ARR) {
                for (auto& el : cur->arr)
                    if (el.type == Json::OBJ || el.type == Json::ARR)
                        stack.push_back(&el);
            }
        }

        std::sort(mon.workspaces.begin(), mon.workspaces.end(),
                  [](const WorkspaceInfo& a, const WorkspaceInfo& b) { return a.name < b.name; });
        monitors.push_back(std::move(mon));
    }
    return true;
}

// =====================================================================
// ICON CACHE
// =====================================================================

struct SHFILEINFOW_COMPAT {
    HICON hIcon;
    int   iIcon;
    DWORD dwAttributes;
    WCHAR szDisplayName[260];
    WCHAR szTypeName[80];
};

class IconCache {
    std::unordered_map<std::string, HICON> m_cache;
    std::unordered_map<std::string, int> m_fails;

public:
    HICON get(const std::string& processName) {
        if (processName.empty()) return nullptr;
        auto it = m_cache.find(processName);
        if (it != m_cache.end()) return it->second;
        if (m_fails[processName] >= 3) return nullptr;

        HICON icon = extractIcon(processName);
        if (icon) {
            m_cache[processName] = icon;
        } else {
            m_fails[processName]++;
        }
        return icon;
    }

    void clear() {
        for (auto& [_, h] : m_cache) if (h) DestroyIcon(h);
        m_cache.clear();
        m_fails.clear();
    }

private:
    std::wstring findExePath(const std::string& procName) {
        std::wstring target = Utf8ToWide(procName);
        // Append .exe if needed
        if (target.size() < 4 ||
            _wcsicmp(target.c_str() + target.size() - 4, L".exe") != 0)
            target += L".exe";

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return {};

        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, target.c_str()) == 0) {
                    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        WCHAR path[MAX_PATH];
                        DWORD sz = MAX_PATH;
                        if (QueryFullProcessImageNameW(hProc, 0, path, &sz)) {
                            CloseHandle(hProc);
                            CloseHandle(snap);
                            return path;
                        }
                        CloseHandle(hProc);
                    }
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return {};
    }

    HICON extractIcon(const std::string& procName) {
        std::wstring exePath = findExePath(procName);
        if (exePath.empty()) return nullptr;

        // Method 1: SHGetFileInfoW
        SHFILEINFOW_COMPAT sfi = {};
        DWORD_PTR result = SHGetFileInfoW(exePath.c_str(), 0, (SHFILEINFOW*)&sfi,
                                           sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON);
        if (result && sfi.hIcon) return sfi.hIcon;

        // Method 2: ExtractIconExW
        HICON hLarge = nullptr;
        if (ExtractIconExW(exePath.c_str(), 0, &hLarge, nullptr, 1) > 0 && hLarge)
            return hLarge;

        return nullptr;
    }
};

// =====================================================================
// GLOBAL STATE
// =====================================================================

static Settings             g_settings;
static std::vector<MonitorInfo> g_monitors;
static std::string          g_currentWs = "?";
static int                  g_windowCount = 0;
static int                  g_errorCount = 0;
static std::mutex           g_stateMutex;

static WsClient             g_eventWs;
static WsClient             g_cmdWs;
static std::mutex           g_cmdMutex;

static std::vector<HWND>    g_widgets;
static std::vector<HitRect> g_hitRects;
static IconCache            g_iconCache;

static std::atomic<bool>    g_running{false};
static std::thread          g_eventThread;
static bool                 g_barHidden = false;

static HFONT                g_fontBold = nullptr;
static HFONT                g_fontSmall = nullptr;

// =====================================================================
// SETTINGS LOADING
// =====================================================================

static void LoadSettings() {
    PCWSTR s;

    s = Wh_GetStringSetting(L"wsUrl");
    g_settings.wsUrl = s;
    std::string url = WideToUtf8(s);
    Wh_FreeStringSetting(s);

    // Parse host:port from ws://host:port
    g_settings.wsHost = "127.0.0.1";
    g_settings.wsPort = 6123;
    size_t pos = url.find("://");
    if (pos != std::string::npos) {
        std::string hp = url.substr(pos + 3);
        size_t colon = hp.find(':');
        if (colon != std::string::npos) {
            g_settings.wsHost = hp.substr(0, colon);
            g_settings.wsPort = atoi(hp.substr(colon + 1).c_str());
        } else {
            g_settings.wsHost = hp;
        }
    }

    g_settings.autoToggle   = Wh_GetIntSetting(L"autoToggleTiling");
    g_settings.transparent  = Wh_GetIntSetting(L"transparent");
    g_settings.iconsOnly    = Wh_GetIntSetting(L"iconsOnly");
    g_settings.positionRight = Wh_GetIntSetting(L"positionRight");
    g_settings.labelLeft    = Wh_GetIntSetting(L"labelLeft");
    g_settings.workspaceGap = Wh_GetIntSetting(L"workspaceGap");
    g_settings.debounceMs   = Wh_GetIntSetting(L"debounceMs");
    
    
    if (g_settings.debounceMs < 50) g_settings.debounceMs = 300;

    auto loadColor = [](const wchar_t* name, COLORREF def) -> COLORREF {
        PCWSTR v = Wh_GetStringSetting(name);
        std::string hex = WideToUtf8(v);
        Wh_FreeStringSetting(v);
        return hex.empty() ? def : HexToColor(hex.c_str());
    };

    g_settings.bgColor       = loadColor(L"bgColor", RGB(20, 20, 20));
    g_settings.textColor     = loadColor(L"textColor", RGB(255, 255, 255));
    g_settings.activeColor   = loadColor(L"activeColor", RGB(66, 192, 251));
    g_settings.inactiveColor = loadColor(L"inactiveColor", RGB(100, 100, 100));
    g_settings.errorColor    = loadColor(L"errorColor", RGB(255, 100, 100));

    // Override auto-toggle from local storage if toggled via menu
    int stored = Wh_GetIntValue(L"autoToggleOverride", -1);
    if (stored >= 0) g_settings.autoToggle = (stored != 0);
}

static void CreateFonts() {
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    g_fontBold = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontSmall = CreateFontW(-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

// =====================================================================
// GLAZEWM COMMANDS
// =====================================================================

static bool GlazeQuery(const std::string& msg, std::string& response) {
    std::lock_guard<std::mutex> lock(g_cmdMutex);
    if (!g_cmdWs.isOpen()) {
        std::wstring host = Utf8ToWide(g_settings.wsHost);
        if (!g_cmdWs.connect(host, g_settings.wsPort))
            return false;
    }
    if (!g_cmdWs.send(msg)) {
        g_cmdWs.close();
        return false;
    }
    response = g_cmdWs.receive();
    if (response.empty()) {
        g_cmdWs.close();
        return false;
    }
    return true;
}

static void GlazeCommand(const std::string& cmd) {
    std::string resp;
    GlazeQuery("command " + cmd, resp);
}

static void GlazeCommandAsync(const std::string& cmd) {
    std::thread([cmd]() { GlazeCommand(cmd); }).detach();
}

static bool QueryAndUpdateState() {
    std::string resp;
    if (!GlazeQuery("query monitors", resp)) {
        g_errorCount++;
        return false;
    }

    Json json = Json::parse(resp);
    std::vector<MonitorInfo> monitors;
    std::string currentWs;
    int windowCount = 0;

    if (!ParseMonitors(json, monitors, currentWs, windowCount)) {
        g_errorCount++;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_monitors = std::move(monitors);
        g_currentWs = currentWs;
        g_windowCount = windowCount;
        g_errorCount = 0;
    }
    return true;
}

// =====================================================================
// FULLSCREEN DETECTION
// =====================================================================

struct MONITORINFO_COMPAT {
    DWORD cbSize;
    RECT  rcMonitor;
    RECT  rcWork;
    DWORD dwFlags;
};

static bool IsFullscreenActive() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    WCHAR cls[256];
    GetClassNameW(fg, cls, 256);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0)
        return false;

    RECT wr;
    GetWindowRect(fg, &wr);

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (!mon) return false;

    MONITORINFO_COMPAT mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, (MONITORINFO*)&mi);

    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

// =====================================================================
// WIDGET POSITIONING
// =====================================================================

static void PositionWidget(HWND hwnd, int contentWidth) {
    if (!hwnd) return;
    HWND taskbarHwnd = (HWND)GetWindowLongPtrW(hwnd, GWLP_HWNDPARENT);
    if (!taskbarHwnd) return;

    RECT taskbarRect;
    GetWindowRect(taskbarHwnd, &taskbarRect);

    int width = std::max(contentWidth, 60);
    int x;

    if (g_settings.positionRight) {
        // Find tray notification area
        HWND tray = FindWindowExW(taskbarHwnd, nullptr, L"TrayNotifyWnd", nullptr);
        if (!tray) {
            // Windows 11: try clock
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
        // Put it on the left (e.g. after Start button or just left margin)
        // If they have center icons, left is totally empty!
        x = taskbarRect.left + 4;
    }

    MoveWindow(hwnd, x, taskbarRect.top, width, taskbarRect.bottom - taskbarRect.top, TRUE);
}

// =====================================================================
// WIDGET RENDERING (GDI)
// =====================================================================

static void PaintWidget(HWND hwnd) {
    RECT cr;
    GetClientRect(hwnd, &cr);
    int W = cr.right, H = cr.bottom;
    if (W <= 0 || H <= 0) return;

    HDC hdc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

    // Background
    COLORREF bgCol = g_settings.transparent ? TRANSPARENT_KEY_COLOR : g_settings.bgColor;
    HBRUSH bgBrush = CreateSolidBrush(bgCol);
    FillRect(mem, &cr, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(mem, TRANSPARENT);
    HFONT oldFont = (HFONT)SelectObject(mem, g_fontBold);

    auto it = std::remove_if(g_hitRects.begin(), g_hitRects.end(), [hwnd](const HitRect& hr) { return hr.hwnd == hwnd; });
    g_hitRects.erase(it, g_hitRects.end());

    std::lock_guard<std::mutex> lock(g_stateMutex);

    // Find which monitor this widget is on
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO_COMPAT mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, (MONITORINFO*)&mi);

    const MonitorInfo* targetMon = nullptr;
    for (auto& m : g_monitors) {
        if (m.x == mi.rcMonitor.left && m.y == mi.rcMonitor.top) {
            targetMon = &m;
            break;
        }
    }
    // Fallback if coordinates don't perfectly match
    if (!targetMon && !g_monitors.empty()) targetMon = &g_monitors[0];

    std::vector<const WorkspaceInfo*> allWs;
    if (targetMon) {
        for (auto& ws : targetMon->workspaces) {
            if (ws.hasWindows || ws.focused) {
                allWs.push_back(&ws);
            }
        }
    }

    if (allWs.empty()) {
        // Error or no data
        COLORREF col = g_errorCount > 3 ? g_settings.errorColor : g_settings.textColor;
        const wchar_t* txt = g_errorCount > 3 ? L"!" : L"?";
        SetTextColor(mem, col);
        SelectObject(mem, g_fontBold);
        TextOutW(mem, 8, (H - 16) / 2, txt, 1);
        goto done;
    }

    {
        int x = 6; // padding
        int iconSize = 16;
        int iconY = (H - iconSize) / 2;
        int sepPad = g_settings.workspaceGap;

        for (size_t wi = 0; wi < allWs.size(); wi++) {
            const WorkspaceInfo* ws = allWs[wi];

            // Separator
            if (wi > 0) {
                HPEN pen = CreatePen(PS_SOLID, 1, g_settings.inactiveColor);
                HPEN oldPen = (HPEN)SelectObject(mem, pen);
                MoveToEx(mem, x + sepPad, 4, nullptr);
                LineTo(mem, x + sepPad, H - 4);
                SelectObject(mem, oldPen);
                DeleteObject(pen);
                x += 1 + sepPad * 2;
            }

            // Workspace number label
            std::wstring nameW = Utf8ToWide(ws->name);
            SelectObject(mem, g_fontBold);

            SIZE numSize;
            GetTextExtentPoint32W(mem, nameW.c_str(), (int)nameW.size(), &numSize);
            int numW = numSize.cx + 8; // padding
            int numH = numSize.cy;
            int numY = (H - numH) / 2;

            // Active highlight background
            if (ws->focused) {
                RECT hlRect = { x, 2, x + numW, H - 2 };
                HBRUSH hlBrush = CreateSolidBrush(g_settings.activeColor);
                FillRect(mem, &hlRect, hlBrush);
                DeleteObject(hlBrush);
            }

            COLORREF numCol = (ws->hasWindows || ws->focused) ? g_settings.textColor : g_settings.inactiveColor;
            SetTextColor(mem, numCol);

            auto drawNumber = [&](int drawX) {
                TextOutW(mem, drawX + 4, numY, nameW.c_str(), (int)nameW.size());
                HitRect hr;
                hr.rect = { drawX, 0, drawX + numW, H };
                hr.type = HitRect::WORKSPACE;
                hr.hwnd = hwnd;
                hr.target = ws->name;
                hr.wsName = ws->name;
                hr.windowFocused = false;
                hr.windowState = "";
                g_hitRects.push_back(hr);
            };

            auto drawIcons = [&](int startX) -> int {
                int ix = startX;
                for (auto& win : ws->windows) {
                    HICON icon = g_iconCache.get(win.process);
                    if (icon) {
                        DrawIconEx(mem, ix, iconY, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
                    } else {
                        // Fallback: draw a small gray circle with first letter
                        HBRUSH fb = CreateSolidBrush(RGB(80, 80, 80));
                        RECT fbR = { ix + 1, iconY + 1, ix + iconSize - 1, iconY + iconSize - 1 };
                        HRGN rgn = CreateEllipticRgnIndirect(&fbR);
                        FillRgn(mem, rgn, fb);
                        DeleteObject(rgn);
                        DeleteObject(fb);
                        std::wstring letter = win.process.empty() ? L"?" : Utf8ToWide(win.process.substr(0, 1));
                        for (auto& c : letter) c = towupper(c);
                        SetTextColor(mem, g_settings.textColor);
                        SelectObject(mem, g_fontSmall);
                        TextOutW(mem, ix + 3, iconY + 1, letter.c_str(), 1);
                        SelectObject(mem, g_fontBold);
                    }

                    // Hit rect for this window
                    HitRect hr;
                    hr.rect = { ix, 0, ix + iconSize + 2, H };
                    hr.type = HitRect::WINDOW;
                    hr.hwnd = hwnd;
                    hr.target = win.id;
                    hr.wsName = ws->name;
                    hr.windowFocused = win.hasFocus;
                    hr.windowState = win.state;
                    g_hitRects.push_back(hr);

                    ix += iconSize + 2;

                    if (!g_settings.iconsOnly && !win.process.empty()) {
                        // Short text label
                        SelectObject(mem, g_fontSmall);
                        std::string display = win.title.empty() ? win.process : win.title;
                        // Strip common suffixes
                        for (auto& suffix : {" - Google Chrome", " - Chrome", " \xe2\x80\x94 Mozilla Firefox",
                                             " - Microsoft Edge", " - Notepad", " - Visual Studio Code"}) {
                            size_t pos = display.rfind(suffix);
                            if (pos != std::string::npos && pos + strlen(suffix) == display.size()) {
                                display = display.substr(0, pos);
                                break;
                            }
                        }
                        if (display.size() > 12) display = display.substr(0, 12);
                        std::wstring dispW = Utf8ToWide(display);
                        SetTextColor(mem, g_settings.textColor);
                        TextOutW(mem, ix, (H - 10) / 2, dispW.c_str(), (int)dispW.size());
                        SIZE ts;
                        GetTextExtentPoint32W(mem, dispW.c_str(), (int)dispW.size(), &ts);
                        ix += ts.cx + 4;
                        SelectObject(mem, g_fontBold);
                    }
                }
                return ix;
            };

            if (g_settings.labelLeft) {
                drawNumber(x);
                x += numW;
                x = drawIcons(x);
            } else {
                int iconStart = x;
                x = drawIcons(x);
                drawNumber(x);
                x += numW;
            }
        }

        // Finalize: resize window to fit content
        int totalWidth = x + 6;
        PositionWidget(hwnd, totalWidth);
    }

done:
    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, hdc);
}

// =====================================================================
// CONTEXT MENU
// =====================================================================

static void ShowContextMenu(HWND hwnd, POINT pt) {
    HMENU menu = CreatePopupMenu();

    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"\x2500\x2500\x2500 Workspaces \x2500\x2500\x2500");

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        int idx = 0;
        for (auto& m : g_monitors) {
            for (auto& ws : m.workspaces) {
                std::wstring label = (ws.hasWindows ? L"\x25CF " : L"\x25CB ") + Utf8ToWide(ws.name);
                UINT flags = MF_STRING;
                if (ws.focused) flags |= MF_CHECKED;
                AppendMenuW(menu, flags, ID_WS_BASE + idx, label.c_str());

                for (auto& win : ws.windows) {
                    std::string t = win.title.empty() ? win.process : win.title;
                    if (t.size() > 40) t = t.substr(0, 37) + "...";
                    std::wstring wt = L"    \x2514 " + Utf8ToWide(t);
                    AppendMenuW(menu, MF_STRING, ID_WS_BASE + idx, wt.c_str());
                }
                idx++;
            }
        }
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TOGGLE_FLOAT, L"Toggle Floating");
    AppendMenuW(menu, MF_STRING, ID_TOGGLE_TILE, L"Toggle Tiling (Alt+V)");
    AppendMenuW(menu, MF_STRING, ID_CLOSE_WIN, L"Close Window");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_settings.autoToggle ? MF_CHECKED : 0),
                ID_AUTO_TOGGLE, L"Auto-Toggle on New Window");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_REDRAW, L"Redraw Windows");
    AppendMenuW(menu, MF_STRING, ID_RELOAD, L"Reload GlazeWM");

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd >= ID_WS_BASE) {
        // Focus workspace by index
        int idx = cmd - ID_WS_BASE;
        int cur = 0;
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (auto& m : g_monitors) {
            for (auto& ws : m.workspaces) {
                if (cur == idx) {
                    GlazeCommandAsync("focus --workspace " + ws.name);
                    return;
                }
                cur++;
            }
        }
    } else {
        switch (cmd) {
            case ID_TOGGLE_FLOAT: GlazeCommandAsync("toggle-floating"); break;
            case ID_TOGGLE_TILE:  GlazeCommandAsync("toggle-tiling-direction"); break;
            case ID_CLOSE_WIN:    GlazeCommandAsync("close"); break;
            case ID_REDRAW:       GlazeCommandAsync("wm-redraw"); break;
            case ID_RELOAD:       GlazeCommandAsync("reload-config"); break;
            case ID_AUTO_TOGGLE:
                g_settings.autoToggle = !g_settings.autoToggle;
                Wh_SetIntValue(L"autoToggleOverride", g_settings.autoToggle ? 1 : 0);
                Wh_Log(L"Auto-toggle tiling: %s", g_settings.autoToggle ? L"enabled" : L"disabled");
                break;
        }
    }
}

// =====================================================================
// WIDGET WINDOW PROCEDURE
// =====================================================================

static LRESULT CALLBACK WidgetWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, TIMER_FULLSCREEN, 1000, nullptr);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        PaintWidget(hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam);
        POINT pt = { mx, my };
        for (auto& hr : g_hitRects) {
            if (hr.hwnd != hwnd) continue;
            if (PtInRect(&hr.rect, pt)) {
                if (hr.type == HitRect::WORKSPACE) {
                    GlazeCommandAsync("focus --workspace " + hr.wsName);
                } else if (hr.type == HitRect::WINDOW) {
                    // Toggle window: minimize if focused, restore if minimized, focus otherwise
                    std::string wid = hr.target;
                    std::string wsName = hr.wsName;
                    bool focused = hr.windowFocused;
                    std::string state = hr.windowState;
                    std::thread([wid, wsName, focused, state]() {
                        if (state == "minimized") {
                            GlazeCommand("focus --workspace " + wsName);
                            Sleep(50);
                            GlazeCommand("--id " + wid + " toggle-minimized");
                        } else if (focused) {
                            GlazeCommand("set-minimized");
                        } else {
                            GlazeCommand("focus --workspace " + wsName);
                            Sleep(50);
                            if (!wid.empty())
                                GlazeCommand("focus --container-id " + wid);
                        }
                    }).detach();
                }
                break;
            }
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT pt;
        GetCursorPos(&pt);
        ShowContextMenu(hwnd, pt);
        return 0;
    }

    case WM_GLAZE_EVENT: {
        bool immediate = (wParam != 0);
        KillTimer(hwnd, TIMER_DEBOUNCE);
        SetTimer(hwnd, TIMER_DEBOUNCE, immediate ? 10 : (UINT)g_settings.debounceMs, nullptr);
        return 0;
    }

    case WM_GLAZE_REFRESH:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_DEBOUNCE) {
            KillTimer(hwnd, TIMER_DEBOUNCE);
            std::thread([hwnd]() {
                if (QueryAndUpdateState())
                    for (auto w : g_widgets) PostMessage(w, WM_GLAZE_REFRESH, 0, 0);
            }).detach();
        } else if (wParam == TIMER_FULLSCREEN) {
            for (auto w : g_widgets) {
                RECT cr; GetClientRect(w, &cr);
                PositionWidget(w, cr.right);
            }
            if (IsFullscreenActive()) {
                if (!g_barHidden) {
                    ShowWindow(hwnd, SW_HIDE);
                    g_barHidden = true;
                }
            } else {
                if (g_barHidden) {
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    g_barHidden = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_FULLSCREEN);
        KillTimer(hwnd, TIMER_DEBOUNCE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// =====================================================================
// WIDGET CREATION
// =====================================================================

static std::thread          g_uiThread;

// =====================================================================
// UI THREAD
// =====================================================================

static void UIThreadProc() {
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
                                     WS_POPUP, 0, 0, 300, 32,
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
}

// =====================================================================
// EVENT THREAD
// =====================================================================

static void EventThreadProc() {
    Wh_Log(L"Event thread started");

    while (g_running.load()) {
        try {
            // Wait for UI window to be ready
            while (g_running.load() && g_widgets.empty()) {
                Sleep(100);
            }
            if (!g_running.load()) break;

            // Connect
            std::wstring host = Utf8ToWide(g_settings.wsHost);
            if (!g_eventWs.connect(host, g_settings.wsPort)) {
                g_errorCount++;
                for (auto w : g_widgets) PostMessage(w, WM_GLAZE_REFRESH, 0, 0);
                Wh_Log(L"Failed to connect to GlazeWM, retrying in 2s...");
                Sleep(2000);
                continue;
            }

            // Subscribe to events
            std::string subMsg = "sub -e";
            for (auto& e : SUBSCRIBE_EVENTS) {
                subMsg += " ";
                subMsg += e;
            }
            if (!g_eventWs.send(subMsg)) {
                g_eventWs.close();
                Sleep(2000);
                continue;
            }

            // Read subscription ack
            std::string ack = g_eventWs.receive();
            if (ack.empty()) {
                g_eventWs.close();
                Sleep(2000);
                continue;
            }

            Json ackJson = Json::parse(ack);
            if (!ackJson["success"].boolean()) {
                Wh_Log(L"Subscription failed");
                g_eventWs.close();
                Sleep(2000);
                continue;
            }

            Wh_Log(L"Connected to GlazeWM event stream");

            // Initial query
            if (QueryAndUpdateState()) {
                for (auto w : g_widgets) PostMessage(w, WM_GLAZE_REFRESH, 0, 0);
            }

            // Event loop
            while (g_running.load()) {
                std::string raw = g_eventWs.receive();
                if (raw.empty()) break; // disconnected

                Json event = Json::parse(raw);
                std::string eventType;
                if (event.has("data"))
                    eventType = event["data"]["eventType"].str();

                bool immediate = IsImmediateEvent(eventType);

                // Auto-toggle tiling on new window
                if (g_settings.autoToggle && eventType == "window_managed") {
                    Wh_Log(L"New window managed, auto-toggling tiling");
                    GlazeCommandAsync("toggle-tiling-direction");
                }

                // Notify widget
                if (!g_widgets.empty())
                    PostMessage(g_widgets[0], WM_GLAZE_EVENT, immediate ? 1 : 0, 0);
            }

            if (g_running.load()) {
                g_errorCount++;
                for (auto w : g_widgets) PostMessage(w, WM_GLAZE_REFRESH, 0, 0);
                Wh_Log(L"Event stream disconnected, reconnecting in 2s...");
            }

        } catch (...) {
            Wh_Log(L"Event thread exception");
        }

        g_eventWs.close();
        if (g_running.load()) Sleep(2000);
    }

    Wh_Log(L"Event thread stopped");
}

// =====================================================================
// MOD LIFECYCLE
// =====================================================================

BOOL Wh_ModInit() {
    Wh_Log(L"GlazeWM Taskbar Workspaces: initializing");

    if (!LoadWinHttpWebSocket()) {
        Wh_Log(L"Failed to load WinHTTP WebSocket functions");
        return FALSE;
    }

    LoadSettings();
    CreateFonts();

    g_running = true;
    g_uiThread = std::thread(UIThreadProc);
    g_eventThread = std::thread(EventThreadProc);

    Wh_Log(L"GlazeWM Taskbar Workspaces: initialized successfully");
    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"GlazeWM Taskbar Workspaces: shutting down");

    g_running = false;

    // Quit UI thread
    for (auto w : g_widgets) PostMessage(w, WM_CLOSE, 0, 0);
    
    if (g_uiThread.joinable()) {
        g_uiThread.join();
    }

    g_eventWs.abort();
    {
        std::lock_guard<std::mutex> lock(g_cmdMutex);
        g_cmdWs.close();
    }

    if (g_eventThread.joinable()) {
        g_eventThread.join();
    }

    g_eventWs.close();

    // Clean up resources
    g_iconCache.clear();
    if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = nullptr; }
    if (g_fontSmall) { DeleteObject(g_fontSmall); g_fontSmall = nullptr; }

    g_monitors.clear();
    g_hitRects.clear();

    Wh_Log(L"GlazeWM Taskbar Workspaces: shutdown complete");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Settings changed, reloading");

    std::string oldHost = g_settings.wsHost;
    int oldPort = g_settings.wsPort;

    LoadSettings();
    CreateFonts();

    // Update transparency mode
    for (auto w : g_widgets) {
        if (g_settings.transparent) {
            SetLayeredWindowAttributes(w, TRANSPARENT_KEY_COLOR, 0, LWA_COLORKEY);
        } else {
            SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);
        }
        InvalidateRect(w, nullptr, TRUE);
    }

    // If URL changed, force reconnect
    if (oldHost != g_settings.wsHost || oldPort != g_settings.wsPort) {
        Wh_Log(L"WebSocket URL changed, reconnecting...");
        g_eventWs.abort(); // This will cause the event thread to reconnect
    }
}
