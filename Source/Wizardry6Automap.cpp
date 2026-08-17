// Wizardry6Automap.cpp
// Wizardry 6 PC-9801 external automap
// Original Wizardry 6 automap behavior (visdata 0/1/2, dark zones,
// water, stairs, fountain and portcullis rules) is based on the
// KoriTama Wizardry 6 Automap Mod, distributed under GPL v2 or later.


#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cwchar>
#include <cwctype>
#include <climits>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <array>
#include <utility>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static const wchar_t* APP_CLASS = L"Wizardry6AutomapWindow";
static const wchar_t* APP_TITLE = L"Wizardry 6 Automap";
static const wchar_t* CONFIG_DIRECTORY = L"Config";
static const wchar_t* CONFIG_PATH = L"Config\\Wizardry6Automap.conf";
static const wchar_t* VISITED_PATH = L"Config\\Wizardry6Automap_visited.bin";
static const wchar_t* NOTES_PATH = L"Config\\Wizardry6Automap_notes.bin";
static const wchar_t* NOTES_TEMP_PATH = L"Config\\Wizardry6Automap_notes.tmp";
static const wchar_t* NOTE_DIALOG_CLASS = L"Wizardry6AutomapNoteDialog";
static const wchar_t* NOTE_TOOLTIP_CLASS = L"Wizardry6AutomapNoteTooltip";

static const UINT_PTR MAIN_TIMER_ID = 1;

// Stable signatures used by the WROOT/WMAZE RAM anchor scan.
static const uint8_t WROOT_LOAD_SIG[] = {
    0x55,0x8B,0xEC,0x83,0xC4,0xFC,0xC7,0x46,
    0xFE,0x00,0x00,0x8B,0x46,0x04,0xE9,0xA7,
    0x00,0xC7,0x46,0xFC,0x00,0x00,0x8B,0x46,
    0xFC,0xD1,0xE0,0xD1,0xE0,0x8B,0xD8,0x83
};
static const uint8_t WMAZE_NAME[] = { 'W','M','A','Z','E' };

struct Config {
    bool enable = true;
    int windowWidth = 512;
    int windowHeight = 512;
    int windowX = -1;
    int windowY = -1;
    std::wstring legacyProcessName;
    std::array<std::wstring, 16> processNames = {};
    uintptr_t manualDataSeg = 0;
    uintptr_t maxScanAddress = 0x80000000ull;
    bool autoSave = true;
    bool invertY = true;
    bool revealAll = false;      // Reveal decoded map without changing global visited state
    bool hideInDarkZones = true; // Original Wizardry 6 Automap Mod behavior
    bool showVisibleNeighbors = true; // Original visdata value 2 behavior
    int pollIntervalMs = 33;
    size_t snapshotBytes = 65536; // One ReadProcessMemory call per normal polling cycle
    int offsets[8] = {
        0x36A6, // state      PC-98 WMAZE.OVR: cmp word ptr [36A6]
        0x36A8, // level/map  PC-98 WMAZE.OVR: push [36A8]
        0x57A6, // dir        PC-98 WMAZE.OVR turn code, mod 4
        0x57A8, // quadrant
        0x57AA, // qY
        0x57AC, // qX
        0x57B4, // qmap data pointer
        0x57B6  // map data pointer
    };
    int offsetTile = 0x56F4; // quadrant*64 + qY*8 + qX
    // Default follows the original Wizardry 6 Automap Mod cursor mapping.
    // Can be changed in ini: dir0=up, dir1=right, dir2=down, dir3=left
    POINT dirVec[4] = { {0,-1}, {1,0}, {0,1}, {-1,0} };
};

enum PosIndex { I_STATE=0, I_LEVEL=1, I_DIR=2, I_QUAD=3, I_QY=4, I_QX=5, I_QMAP=6, I_MAP=7 };

struct Pos {
    uint16_t state = 0;
    uint16_t level = 0;
    uint16_t dir = 0;
    uint16_t quad = 0;
    uint16_t qy = 0;
    uint16_t qx = 0;
    uint16_t qmap = 0;
    uint16_t map = 0;
    uint16_t tile = 0;
    uint16_t absY = 0;
    uint16_t absX = 0;
};


static const COLORREF DEFAULT_NOTE_COLOR = RGB(255, 70, 70);

struct MapNote {
    uint8_t level = 0;
    uint8_t quadrant = 0;
    uint8_t qx = 0;
    uint8_t qy = 0;
    COLORREF color = DEFAULT_NOTE_COLOR;
    std::wstring text;
};

struct NoteHoverTarget {
    int level = -1;
    int quadrant = -1;
    int qx = -1;
    int qy = -1;
    std::wstring text;

    bool IsValid() const
    {
        return level >= 0 && quadrant >= 0 && qx >= 0 && qy >= 0;
    }
};

static Config g_cfg;
static HWND g_hwnd = nullptr;
static HANDLE g_proc = nullptr;
static DWORD g_pid = 0;
static std::wstring g_activeProcessName;
static bool g_attached = false;
static uintptr_t g_dataSeg = 0;
static Pos g_pos;
static bool g_posValid = false;
static bool g_learning = false;
static DWORD g_dirtySinceTick = 0;
static std::wstring g_status = L"Waiting for target process";
static uint8_t g_visited[16][12][8][8] = {};
static std::vector<MapNote> g_notes;
static bool g_notesDirty = false;
static HWND g_noteTooltip = nullptr;
static HFONT g_noteTooltipFont = nullptr;
static NoteHoverTarget g_noteHoverTarget;
static std::wstring g_noteTooltipText;
static bool g_noteTooltipVisible = false;
static bool g_trackingMouseLeave = false;
static uint32_t g_totalSteps = 0;
static int g_cacheMissCount = 0;

static const int W6_LEVEL_COUNT = 16;
static const int W6_QUADRANT_COUNT = 12;
static const int W6_TILES_PER_QUADRANT = 64;
static const int W6_WALL_BYTES_PER_LEVEL = W6_QUADRANT_COUNT * W6_TILES_PER_QUADRANT * 2 / 8;
static const int W6_FEATURE_BYTES_PER_LEVEL = W6_QUADRANT_COUNT * W6_TILES_PER_QUADRANT * 4 / 8;
static const int W6_FEATURE_DIR_BYTES_PER_LEVEL = W6_QUADRANT_COUNT * W6_TILES_PER_QUADRANT * 2 / 8;
static const int W6_FLOOR_BYTES_PER_LEVEL = W6_QUADRANT_COUNT * W6_TILES_PER_QUADRANT / 8;

static uint8_t g_cache_qsx[W6_LEVEL_COUNT][W6_QUADRANT_COUNT] = {};
static uint8_t g_cache_qsy[W6_LEVEL_COUNT][W6_QUADRANT_COUNT] = {};
static uint8_t g_cache_hwalls[W6_LEVEL_COUNT][W6_WALL_BYTES_PER_LEVEL] = {};
static uint8_t g_cache_vwalls[W6_LEVEL_COUNT][W6_WALL_BYTES_PER_LEVEL] = {};
static uint8_t g_cache_features[W6_LEVEL_COUNT][W6_FEATURE_BYTES_PER_LEVEL] = {};
static uint8_t g_cache_features_dirs[W6_LEVEL_COUNT][W6_FEATURE_DIR_BYTES_PER_LEVEL] = {};
static uint8_t g_cache_floor[W6_LEVEL_COUNT][W6_FLOOR_BYTES_PER_LEVEL] = {};
static uint8_t g_cache_roof[W6_LEVEL_COUNT][W6_FLOOR_BYTES_PER_LEVEL] = {};
static bool g_cache_valid[W6_LEVEL_COUNT] = {};
static uint16_t g_cache_map_offset[W6_LEVEL_COUNT] = {};
static uint16_t g_cache_qmap_offset[W6_LEVEL_COUNT] = {};
static bool g_dirty = false;
static int g_invalidCount = 0;
static bool g_inGame = false;
static bool g_currentDark = false;
static bool g_autoReconnectRequested = false;

// View state.  The original Wizardry 6 Automap Mod used 22-pixel tiles and allowed
// the user to drag the map.  These offsets are in client pixels.
static int g_mapScrollX = 0;
static int g_mapScrollY = 0;
static bool g_linkViewActive = false;
static int g_linkViewLevel = 0;
static int g_linkViewAbsX = 0;
static int g_linkViewAbsY = 0;
static bool g_viewRecenterPending = false;
static bool g_draggingMap = false;
static POINT g_dragLast = { 0, 0 };
static const int MAP_SCROLL_LIMIT = 48 * 256;

static CRITICAL_SECTION g_stateLock;
static bool g_stateLockReady = false;
static HANDLE g_pollThread = nullptr;
static HANDLE g_pollStopEvent = nullptr;
static volatile LONG g_snapshotMessagePending = 0;
static const UINT WM_APP_SNAPSHOT = WM_APP + 0x61;
static const DWORD VISITED_AUTOSAVE_INTERVAL_MS = 120000; // 2 minutes


// The original anchor method searches readable process memory for the loaded
// WROOT code signature, then searches forward from that WROOT for "WMAZE".
// Full WROOT discovery is performed only while unattached and at most once
// every second. Once WROOT is known, the smaller WMAZE window is checked
// once per second until the maze overlay appears.
static const DWORD RAM_ANCHOR_PROBE_INTERVAL_MS = 1000;
static const DWORD RAM_ANCHOR_FULL_SCAN_INTERVAL_MS = 1000;
static const size_t RAM_ANCHOR_MIN_REGION_BYTES = 0x20000;
static const size_t RAM_ANCHOR_WMAZE_SEARCH_BYTES = 0x80000;
static const size_t RAM_ANCHOR_SCAN_CHUNK_BYTES = 0x100000;
static const size_t RAM_ANCHOR_EXPECTED_WROOT_WMAZE_DELTA = 0x102AC;

struct WrootCandidate {
    uintptr_t regionBase = 0;
    size_t regionSize = 0;
    uintptr_t wrootAddress = 0;
};

static std::vector<WrootCandidate> g_wrootCandidates;
static DWORD g_lastAnchorProbeTick = 0;
static DWORD g_lastAnchorFullScanTick = 0;

struct ScopedStateLock {
    ScopedStateLock() { if (g_stateLockReady) EnterCriticalSection(&g_stateLock); }
    ~ScopedStateLock() { if (g_stateLockReady) LeaveCriticalSection(&g_stateLock); }
};

static std::string Trim(std::string s)
{
    auto notspace = [](unsigned char c){ return c > ' '; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}

static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)tolower(c); });
    return s;
}

static uintptr_t ParseIntPtr(const std::string& s)
{
    return (uintptr_t)_strtoui64(s.c_str(), nullptr, 0);
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

static bool ParseDirection(const std::string& v, POINT& out)
{
    std::string s = Lower(Trim(v));
    if (s == "up" || s == "north" || s == "n") { out = {0,-1}; return true; }
    if (s == "right" || s == "east" || s == "e") { out = {1,0}; return true; }
    if (s == "down" || s == "south" || s == "s") { out = {0,1}; return true; }
    if (s == "left" || s == "west" || s == "w") { out = {-1,0}; return true; }
    return false;
}

static bool ParseBool(const std::string& value, bool fallback)
{
    const std::string s = Lower(Trim(value));
    if (s == "true" || s == "yes" || s == "on" || s == "1") return true;
    if (s == "false" || s == "no" || s == "off" || s == "0") return false;
    return fallback;
}

static std::string UnquoteConfigValue(const std::string& value)
{
    std::string s = Trim(value);
    if (s.size() >= 2) {
        const char first = s.front();
        const char last = s.back();
        if ((first == '"' && last == '"') ||
            (first == '\'' && last == '\'')) {
            s = s.substr(1, s.size() - 2);
        }
    }
    return s;
}

static void EnsureDefaultConfig()
{
    CreateDirectoryW(CONFIG_DIRECTORY, nullptr);
    const DWORD attrs = GetFileAttributesW(CONFIG_PATH);
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return;

    const wchar_t* oldConfigPath = L"Config\\automap.conf";
    const DWORD oldConfigAttrs = GetFileAttributesW(oldConfigPath);
    if (oldConfigAttrs != INVALID_FILE_ATTRIBUTES && !(oldConfigAttrs & FILE_ATTRIBUTE_DIRECTORY) &&
        CopyFileW(oldConfigPath, CONFIG_PATH, TRUE)) {
        return;
    }

    std::ofstream f("Config/Wizardry6Automap.conf", std::ios::binary);
    if (!f) {
        return;
    }
    f << "[automap]\r\n"
         "target1=\"anex86.exe\"\r\n"
         "target2=\"np21.exe\"\r\n"
         "target3=\"np21nt.exe\"\r\n"
         "target4=\"np2sx.exe\"\r\n"
         "target5=\"np2sxnt.exe\"\r\n"
         "target6=\"np2nt.exe\"\r\n"
         "target7=\"np2.exe\"\r\n"
         "target8=\"np2w.exe\"\r\n"
         "target9=\"np2x64w.exe\"\r\n"
         "target10=\"np21w.exe\"\r\n"
         "target11=\"np21x64w.exe\"\r\n"
         "target12=\"Next.EXE\"\r\n"
         "target13=\"WIZ6.EXE\"\r\n"
         "target14=\"ウィザードリィ6.EXE\"\r\n"
         "target15=\r\n"
         "target16=\r\n"
         "enable=true\r\n"
         "hide_in_dark_zones=true\r\n"
         "width=512\r\n"
         "height=512\r\n"
         "position_x=-1\r\n"
         "position_y=-1\r\n";
}

static void LoadConfig()
{
    EnsureDefaultConfig();
    std::ifstream f("Config/Wizardry6Automap.conf");
    if (!f) {
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));
        if (key == "enable") g_cfg.enable = ParseBool(val, g_cfg.enable);
        else if (key == "hide_in_dark_zones") g_cfg.hideInDarkZones = ParseBool(val, g_cfg.hideInDarkZones);
        else if (key == "width") {
            int n = atoi(val.c_str());
            if (n == -1 || (n >= 320 && n <= 4096)) g_cfg.windowWidth = n;
        }
        else if (key == "height") {
            int n = atoi(val.c_str());
            if (n == -1 || (n >= 240 && n <= 4096)) g_cfg.windowHeight = n;
        }
        else if (key == "position_x") {
            long n = strtol(val.c_str(), nullptr, 0);
            if (n >= -32768 && n <= 32767) g_cfg.windowX = (int)n;
        }
        else if (key == "position_y") {
            long n = strtol(val.c_str(), nullptr, 0);
            if (n >= -32768 && n <= 32767) g_cfg.windowY = (int)n;
        }
        else if (key == "target" || key == "process") {
            g_cfg.legacyProcessName = Utf8ToWide(UnquoteConfigValue(val));
        }
        else if (key.size() >= 7 && key.compare(0, 6, "target") == 0) {
            const std::string suffix = key.substr(6);
            char* end = nullptr;
            const long targetIndex = strtol(suffix.c_str(), &end, 10);
            if (end && *end == '\0' && targetIndex >= 1 && targetIndex <= 16) {
                g_cfg.processNames[static_cast<size_t>(targetIndex - 1)] =
                    Utf8ToWide(UnquoteConfigValue(val));
            }
        }
        else if (key == "dataseg") g_cfg.manualDataSeg = ParseIntPtr(val);
        else if (key == "max_scan_address") g_cfg.maxScanAddress = ParseIntPtr(val);
        else if (key == "autosave") g_cfg.autoSave = ParseBool(val, g_cfg.autoSave);
        else if (key == "invert_y") g_cfg.invertY = ParseBool(val, g_cfg.invertY);
        else if (key == "reveal_all") g_cfg.revealAll = ParseBool(val, g_cfg.revealAll);
        else if (key == "show_visible_neighbors") g_cfg.showVisibleNeighbors = ParseBool(val, g_cfg.showVisibleNeighbors);
        else if (key == "poll_interval_ms") { int ms = atoi(val.c_str()); if (ms >= 10 && ms <= 1000) g_cfg.pollIntervalMs = ms; }
        else if (key == "snapshot_bytes") { size_t n = (size_t)strtoull(val.c_str(), nullptr, 0); if (n >= 65536 && n <= 1048576) g_cfg.snapshotBytes = n; }
        else if (key == "offset_state") g_cfg.offsets[I_STATE] = (int)ParseIntPtr(val);
        else if (key == "offset_level") g_cfg.offsets[I_LEVEL] = (int)ParseIntPtr(val);
        else if (key == "offset_dir") g_cfg.offsets[I_DIR] = (int)ParseIntPtr(val);
        else if (key == "offset_quad") g_cfg.offsets[I_QUAD] = (int)ParseIntPtr(val);
        else if (key == "offset_qy") g_cfg.offsets[I_QY] = (int)ParseIntPtr(val);
        else if (key == "offset_qx") g_cfg.offsets[I_QX] = (int)ParseIntPtr(val);
        else if (key == "offset_qmap") g_cfg.offsets[I_QMAP] = (int)ParseIntPtr(val);
        else if (key == "offset_map") g_cfg.offsets[I_MAP] = (int)ParseIntPtr(val);
        else if (key == "offset_tile") g_cfg.offsetTile = (int)ParseIntPtr(val);
        else if (key == "dir0") ParseDirection(val, g_cfg.dirVec[0]);
        else if (key == "dir1") ParseDirection(val, g_cfg.dirVec[1]);
        else if (key == "dir2") ParseDirection(val, g_cfg.dirVec[2]);
        else if (key == "dir3") ParseDirection(val, g_cfg.dirVec[3]);
    }
}

static std::vector<std::wstring> GetConfiguredProcessNames()
{
    std::vector<std::wstring> names;

    auto addUnique = [&names](const std::wstring& name) {
        if (name.empty()) return;
        for (const std::wstring& existing : names) {
            if (_wcsicmp(existing.c_str(), name.c_str()) == 0) return;
        }
        names.push_back(name);
    };

    // Legacy target= / process= remains first for backward compatibility.
    addUnique(g_cfg.legacyProcessName);
    for (const std::wstring& name : g_cfg.processNames) addUnique(name);
    return names;
}

static std::wstring GetWaitingProcessLabel()
{
    const std::vector<std::wstring> names = GetConfiguredProcessNames();
    if (names.size() == 1) return names.front();
    return L"configured target process";
}

template <typename FunctionPointer>
static FunctionPointer GetProcAddressTyped(HMODULE module, const char* name) noexcept
{
    static_assert(sizeof(FunctionPointer) == sizeof(FARPROC),
                  "Unexpected Windows function-pointer size");

    const FARPROC raw = GetProcAddress(module, name);
    FunctionPointer typed = nullptr;
    if (raw) std::memcpy(&typed, &raw, sizeof(typed));
    return typed;
}

static void EnableBestDpiAwareness()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    using SetProcessDpiAwarenessContextFn = BOOL (WINAPI*)(HANDLE);
    const auto setContext = GetProcAddressTyped<SetProcessDpiAwarenessContextFn>(
        user32, "SetProcessDpiAwarenessContext");
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4.
    if (setContext && setContext(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)))) return;

    using SetProcessDPIAwareFn = BOOL (WINAPI*)();
    const auto setAware = GetProcAddressTyped<SetProcessDPIAwareFn>(
        user32, "SetProcessDPIAware");
    if (setAware) setAware();
}

static UINT GetWindowDpiCompat(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
        const auto getDpiForWindow = GetProcAddressTyped<GetDpiForWindowFn>(
            user32, "GetDpiForWindow");
        if (getDpiForWindow) {
            const UINT dpi = getDpiForWindow(hwnd);
            if (dpi != 0) return dpi;
        }
    }

    HDC dc = GetDC(hwnd);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96u;
}

static BOOL AdjustWindowRectForDpiCompat(RECT* rect, DWORD style, BOOL hasMenu,
                                         DWORD exStyle, UINT dpi)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using AdjustWindowRectExForDpiFn = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        const auto adjustForDpi = GetProcAddressTyped<AdjustWindowRectExForDpiFn>(
            user32, "AdjustWindowRectExForDpi");
        if (adjustForDpi) return adjustForDpi(rect, style, hasMenu, exStyle, dpi);
    }
    return AdjustWindowRectEx(rect, style, hasMenu, exStyle);
}

static void ApplyInitialWindowPlacement(HWND hwnd, DWORD windowStyle, DWORD exStyle)
{
    RECT defaultWindow = {};
    RECT defaultClient = {};
    GetWindowRect(hwnd, &defaultWindow);
    GetClientRect(hwnd, &defaultClient);

    const int defaultClientWidth = std::max(1, static_cast<int>(defaultClient.right - defaultClient.left));
    const int defaultClientHeight = std::max(1, static_cast<int>(defaultClient.bottom - defaultClient.top));
    const int clientWidth = (g_cfg.windowWidth == -1) ? defaultClientWidth : g_cfg.windowWidth;
    const int clientHeight = (g_cfg.windowHeight == -1) ? defaultClientHeight : g_cfg.windowHeight;
    const int x = (g_cfg.windowX == -1) ? defaultWindow.left : g_cfg.windowX;
    const int y = (g_cfg.windowY == -1) ? defaultWindow.top : g_cfg.windowY;

    // Move first so GetDpiForWindow observes the monitor selected by position_x/y.
    SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    RECT wanted = { 0, 0, clientWidth, clientHeight };
    const UINT dpi = GetWindowDpiCompat(hwnd);
    if (!AdjustWindowRectForDpiCompat(&wanted, windowStyle, FALSE, exStyle, dpi)) {
        return;
    }

    const int outerWidth = wanted.right - wanted.left;
    const int outerHeight = wanted.bottom - wanted.top;
    SetWindowPos(hwnd, nullptr, x, y, outerWidth, outerHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    RECT actualClient = {};
    GetClientRect(hwnd, &actualClient);
}

static bool IsReadableProtection(DWORD protect)
{
    if (protect & PAGE_GUARD) return false;
    if (protect & PAGE_NOACCESS) return false;
    switch (protect & 0xFF) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

static bool FindProcess(DWORD& pid, std::wstring& matchedName)
{
    pid = 0;
    matchedName.clear();

    const std::vector<std::wstring> targets = GetConfiguredProcessNames();
    if (targets.empty()) return false;

    struct ProcessItem {
        DWORD pid = 0;
        std::wstring name;
    };
    std::vector<ProcessItem> processes;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcessItem item;
            item.pid = pe.th32ProcessID;
            item.name = pe.szExeFile;
            processes.push_back(std::move(item));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    // Priority: legacy target= first, then target1= through target16=.
    for (const std::wstring& target : targets) {
        for (const ProcessItem& process : processes) {
            if (_wcsicmp(process.name.c_str(), target.c_str()) == 0) {
                pid = process.pid;
                matchedName = process.name;
                return true;
            }
        }
    }
    return false;
}

static bool ProcessAlive()
{
    if (!g_proc) return false;
    DWORD ec = 0;
    if (!GetExitCodeProcess(g_proc, &ec)) return false;
    return ec == STILL_ACTIVE;
}

static bool ReadExact(uintptr_t addr, void* out, size_t size)
{
    if (!g_proc) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(g_proc, (LPCVOID)addr, out, size, &got) && got == size;
}

static uint16_t U16LE(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool IsStateOk(uint16_t st)
{
    return st == 5 || (st >= 10 && st <= 14) || st == 19 || st == 20 || st == 22 || st == 24;
}

static bool ValidatePos(const Pos& p)
{
    if (!(IsStateOk(p.state) && p.level < 16 && p.dir < 4 && p.quad < 12 && p.qy < 8 && p.qx < 8)) return false;
    // In the PC-98 RAM anchor dump, the PC-98 WMAZE "tile" field
    // did not equal quadrant*64+qY*8+qX. Treat it as diagnostic only.
    return true;
}

static bool ReadPosRawAt(uintptr_t dataSeg, Pos& p)
{
    uint16_t vals[8] = {};
    for (int i = 0; i < 8; ++i) {
        if (!ReadExact(dataSeg + (uintptr_t)g_cfg.offsets[i], &vals[i], 2)) return false;
    }
    p.state = vals[I_STATE];
    p.level = vals[I_LEVEL];
    p.dir   = vals[I_DIR];
    p.quad  = vals[I_QUAD];
    p.qy    = vals[I_QY];
    p.qx    = vals[I_QX];
    p.qmap  = vals[I_QMAP];
    p.map   = vals[I_MAP];
    if (!ReadExact(dataSeg + (uintptr_t)g_cfg.offsetTile, &p.tile, 2)) return false;
    // PC-98 WMAZE absolute Y/X fields; useful as diagnostics outside maze states too.
    if (!ReadExact(dataSeg + 0x57AE, &p.absY, 2)) return false;
    if (!ReadExact(dataSeg + 0x57B0, &p.absX, 2)) return false;
    return true;
}

static size_t FindBytesInBuffer(const uint8_t* b, size_t len, const uint8_t* pat, size_t patLen, size_t start = 0)
{
    if (!b || !pat || patLen == 0 || len < patLen || start > len - patLen) return (size_t)-1;
    for (size_t i = start; i <= len - patLen; ++i) {
        if (memcmp(b + i, pat, patLen) == 0) return i;
    }
    return (size_t)-1;
}

static bool ReadPosFromImage(const uint8_t* image, size_t imageSize, Pos& p);

static void AddWrootCandidate(std::vector<WrootCandidate>& found,
                              uintptr_t regionBase,
                              size_t regionSize,
                              uintptr_t wrootAddress)
{
    for (const WrootCandidate& existing : found) {
        if (existing.wrootAddress == wrootAddress) return;
    }
    WrootCandidate candidate;
    candidate.regionBase = regionBase;
    candidate.regionSize = regionSize;
    candidate.wrootAddress = wrootAddress;
    found.push_back(candidate);
}

static void EnumerateWrootCandidates()
{
    std::vector<WrootCandidate> found;
    if (!g_proc) return;

    uintptr_t addr = 0;
    MEMORY_BASIC_INFORMATION mbi = {};

    while (addr < g_cfg.maxScanAddress &&
           VirtualQueryEx(g_proc, reinterpret_cast<LPCVOID>(addr),
                          &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t next = base + static_cast<uintptr_t>(mbi.RegionSize);
        if (next <= addr) break;
        addr = next;
        if (base >= g_cfg.maxScanAddress) break;
        if (mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect) ||
            mbi.RegionSize < RAM_ANCHOR_MIN_REGION_BYTES) {
            continue;
        }

        const size_t regionSize = static_cast<size_t>(mbi.RegionSize);
        const size_t overlap = sizeof(WROOT_LOAD_SIG) - 1;
        size_t regionOffset = 0;

        while (regionOffset < regionSize) {
            const size_t remaining = regionSize - regionOffset;
            const size_t request = std::min(RAM_ANCHOR_SCAN_CHUNK_BYTES, remaining);
            std::vector<uint8_t> buffer(request);
            SIZE_T got = 0;
            const uintptr_t readAddress = base + regionOffset;
            if (ReadProcessMemory(g_proc, reinterpret_cast<LPCVOID>(readAddress),
                                  buffer.data(), request, &got) &&
                got >= sizeof(WROOT_LOAD_SIG)) {
                size_t search = 0;
                while (search + sizeof(WROOT_LOAD_SIG) <= static_cast<size_t>(got)) {
                    const size_t hit = FindBytesInBuffer(buffer.data(), static_cast<size_t>(got),
                                                         WROOT_LOAD_SIG, sizeof(WROOT_LOAD_SIG),
                                                         search);
                    if (hit == static_cast<size_t>(-1)) break;
                    AddWrootCandidate(found, base, regionSize, readAddress + hit);
                    search = hit + 1;
                }
            }

            if (request == remaining) break;
            regionOffset += request - overlap;
        }
    }

    std::sort(found.begin(), found.end(), [](const WrootCandidate& a,
                                              const WrootCandidate& b) {
        if (a.regionSize != b.regionSize) return a.regionSize > b.regionSize;
        return a.wrootAddress < b.wrootAddress;
    });

    {
        ScopedStateLock lock;
        g_wrootCandidates = found;
        g_lastAnchorFullScanTick = GetTickCount();
    }

}

struct WrootAnchorPair {
    WrootCandidate seed;
    uintptr_t wmazeAddress = 0;
    size_t deltaDistance = 0;
};

static bool ConfirmWrootAnchorPair(const WrootAnchorPair& pair,
                                        uintptr_t& outDataSeg,
                                        Pos& outPos)
{
    if (pair.wmazeAddress < 0x57C) return false;
    const uintptr_t dataSeg = pair.wmazeAddress - 0x57C;

    static thread_local std::vector<uint8_t> snapshot;
    if (snapshot.size() != g_cfg.snapshotBytes) snapshot.resize(g_cfg.snapshotBytes);
    SIZE_T got = 0;
    if (!ReadProcessMemory(g_proc, reinterpret_cast<LPCVOID>(dataSeg),
                           snapshot.data(), snapshot.size(), &got) ||
        got != snapshot.size()) {
        return false;
    }

    Pos p;
    if (!ReadPosFromImage(snapshot.data(), snapshot.size(), p)) {
        return false;
    }

    outDataSeg = dataSeg;
    outPos = p;
    return true;
}

static bool ProbeWrootCandidates(uintptr_t& outDataSeg, Pos& outPos)
{
    std::vector<WrootCandidate> seeds;
    {
        ScopedStateLock lock;
        seeds = g_wrootCandidates;
    }
    if (seeds.empty()) return false;

    std::vector<WrootAnchorPair> pairs;
    size_t liveWrootCount = 0;

    for (const WrootCandidate& seed : seeds) {
        uint8_t signature[sizeof(WROOT_LOAD_SIG)] = {};
        if (!ReadExact(seed.wrootAddress, signature, sizeof(signature)) ||
            memcmp(signature, WROOT_LOAD_SIG, sizeof(signature)) != 0) {
            continue;
        }
        ++liveWrootCount;

        const uintptr_t regionEnd = seed.regionBase + seed.regionSize;
        if (seed.wrootAddress >= regionEnd) continue;
        const size_t available = static_cast<size_t>(regionEnd - seed.wrootAddress);
        const size_t request = std::min(RAM_ANCHOR_WMAZE_SEARCH_BYTES, available);
        if (request < sizeof(WMAZE_NAME)) continue;

        std::vector<uint8_t> window(request);
        SIZE_T got = 0;
        if (!ReadProcessMemory(g_proc, reinterpret_cast<LPCVOID>(seed.wrootAddress),
                               window.data(), request, &got) ||
            got < sizeof(WMAZE_NAME)) {
            continue;
        }

        size_t search = 0;
        while (search + sizeof(WMAZE_NAME) <= static_cast<size_t>(got)) {
            const size_t hit = FindBytesInBuffer(window.data(), static_cast<size_t>(got),
                                                 WMAZE_NAME, sizeof(WMAZE_NAME),
                                                 search);
            if (hit == static_cast<size_t>(-1)) break;
            WrootAnchorPair pair;
            pair.seed = seed;
            pair.wmazeAddress = seed.wrootAddress + hit;
            const size_t delta = hit;
            pair.deltaDistance = delta > RAM_ANCHOR_EXPECTED_WROOT_WMAZE_DELTA
                ? delta - RAM_ANCHOR_EXPECTED_WROOT_WMAZE_DELTA
                : RAM_ANCHOR_EXPECTED_WROOT_WMAZE_DELTA - delta;
            pairs.push_back(pair);
            search = hit + 1;
        }
    }

    if (liveWrootCount == 0) {
        ScopedStateLock lock;
        g_wrootCandidates.clear();
        return false;
    }

    std::sort(pairs.begin(), pairs.end(), [](const WrootAnchorPair& a,
                                              const WrootAnchorPair& b) {
        if (a.deltaDistance != b.deltaDistance) return a.deltaDistance < b.deltaDistance;
        const size_t deltaA = static_cast<size_t>(a.wmazeAddress - a.seed.wrootAddress);
        const size_t deltaB = static_cast<size_t>(b.wmazeAddress - b.seed.wrootAddress);
        return deltaA < deltaB;
    });

    for (const WrootAnchorPair& pair : pairs) {
        if (ConfirmWrootAnchorPair(pair, outDataSeg, outPos)) return true;
    }
    return false;
}

static bool ScanForWrootRamAnchor(uintptr_t& outDataSeg, Pos& outPos)
{
    const DWORD now = GetTickCount();
    bool needFullScan = false;
    {
        ScopedStateLock lock;
        needFullScan = g_lastAnchorFullScanTick == 0 ||
                       now - g_lastAnchorFullScanTick >=
                           RAM_ANCHOR_FULL_SCAN_INTERVAL_MS;
    }

    if (needFullScan) EnumerateWrootCandidates();
    return ProbeWrootCandidates(outDataSeg, outPos);
}

static uint16_t W6GetBits(const uint8_t* a, int index, int bits)
{
    uint16_t v = 0;
    int bit = index * bits;
    for (int i = 0; i < bits; ++i) {
        int bi = bit + i;
        if (a[bi / 8] & (1u << (bi & 7))) v |= (uint16_t)(1u << i);
    }
    return v;
}

struct DecodedLevelSnapshot {
    uint8_t qsx[W6_QUADRANT_COUNT] = {};
    uint8_t qsy[W6_QUADRANT_COUNT] = {};
    uint8_t hwalls[W6_WALL_BYTES_PER_LEVEL] = {};
    uint8_t vwalls[W6_WALL_BYTES_PER_LEVEL] = {};
    uint8_t features[W6_FEATURE_BYTES_PER_LEVEL] = {};
    uint8_t featureDirs[W6_FEATURE_DIR_BYTES_PER_LEVEL] = {};
    uint8_t floorMap[W6_FLOOR_BYTES_PER_LEVEL] = {};
    uint8_t roofMap[W6_FLOOR_BYTES_PER_LEVEL] = {};
};

static bool ImageRangeOk(size_t imageSize, size_t offset, size_t length)
{
    return offset <= imageSize && length <= imageSize - offset;
}

static bool ReadPosFromImage(const uint8_t* image, size_t imageSize, Pos& p)
{
    if (!image) return false;
    auto read16 = [&](size_t offset, uint16_t& out) -> bool {
        if (!ImageRangeOk(imageSize, offset, 2)) return false;
        out = U16LE(image + offset);
        return true;
    };

    uint16_t vals[8] = {};
    for (int i = 0; i < 8; ++i) {
        if (!read16((size_t)g_cfg.offsets[i], vals[i])) return false;
    }
    p.state = vals[I_STATE];
    p.level = vals[I_LEVEL];
    p.dir   = vals[I_DIR];
    p.quad  = vals[I_QUAD];
    p.qy    = vals[I_QY];
    p.qx    = vals[I_QX];
    p.qmap  = vals[I_QMAP];
    p.map   = vals[I_MAP];
    if (!read16((size_t)g_cfg.offsetTile, p.tile)) return false;
    if (!read16(0x57AE, p.absY)) return false;
    if (!read16(0x57B0, p.absX)) return false;
    return true;
}

static bool DecodeMapFromImage(const uint8_t* image, size_t imageSize, const Pos& p, DecodedLevelSnapshot& out)
{
    if (!image || p.level >= W6_LEVEL_COUNT || p.map < 0x100 || p.map > 0xF000) return false;
    const size_t base = (size_t)p.map;
    if (!ImageRangeOk(imageSize, base + 0x060, 0x49A + W6_FLOOR_BYTES_PER_LEVEL - 0x060)) return false;

    memcpy(out.qsx,         image + base + 0x1E0, W6_QUADRANT_COUNT);
    memcpy(out.qsy,         image + base + 0x1EC, W6_QUADRANT_COUNT);
    memcpy(out.hwalls,      image + base + 0x060, W6_WALL_BYTES_PER_LEVEL);
    memcpy(out.vwalls,      image + base + 0x120, W6_WALL_BYTES_PER_LEVEL);
    memcpy(out.features,    image + base + 0x1F8, W6_FEATURE_BYTES_PER_LEVEL);
    memcpy(out.featureDirs, image + base + 0x378, W6_FEATURE_DIR_BYTES_PER_LEVEL);
    memcpy(out.floorMap,    image + base + 0x43A, W6_FLOOR_BYTES_PER_LEVEL);
    memcpy(out.roofMap,     image + base + 0x49A, W6_FLOOR_BYTES_PER_LEVEL);
    return true;
}

static void PublishDecodedLevel(const Pos& p, const DecodedLevelSnapshot& map)
{
    const int lvl = (int)p.level;
    memcpy(g_cache_qsx[lvl],          map.qsx,         sizeof(map.qsx));
    memcpy(g_cache_qsy[lvl],          map.qsy,         sizeof(map.qsy));
    memcpy(g_cache_hwalls[lvl],       map.hwalls,      sizeof(map.hwalls));
    memcpy(g_cache_vwalls[lvl],       map.vwalls,      sizeof(map.vwalls));
    memcpy(g_cache_features[lvl],     map.features,    sizeof(map.features));
    memcpy(g_cache_features_dirs[lvl],map.featureDirs, sizeof(map.featureDirs));
    memcpy(g_cache_floor[lvl],        map.floorMap,    sizeof(map.floorMap));
    memcpy(g_cache_roof[lvl],         map.roofMap,     sizeof(map.roofMap));
    g_cache_valid[lvl] = true;
    g_cache_map_offset[lvl] = p.map;
    g_cache_qmap_offset[lvl] = p.qmap;
}

static bool W6CAbsToQuadrant(int lvl, int absX, int absY, int& quadrant, int& qx, int& qy)
{
    if (lvl < 0 || lvl >= W6_LEVEL_COUNT || !g_cache_valid[lvl]) return false;
    for (int q = 0; q < W6_QUADRANT_COUNT; ++q) {
        const int sx = (int)g_cache_qsx[lvl][q];
        const int sy = (int)g_cache_qsy[lvl][q];
        if (absX >= sx && absX <= sx + 7 && absY >= sy && absY <= sy + 7) {
            quadrant = q;
            qx = absX - sx;
            qy = absY - sy;
            return true;
        }
    }
    return false;
}

static bool W6IsHiddenDarkZone(int lvl, int quadrant, int qx, int qy)
{
    if (!g_cfg.hideInDarkZones) return false;
    if (lvl != 5 && lvl != 12) return false;
    if (quadrant < 0 || quadrant >= W6_QUADRANT_COUNT || qx < 0 || qx >= 8 || qy < 0 || qy >= 8) return false;
    const int index = quadrant * 64 + qy * 8 + qx;
    const uint16_t feature = W6GetBits(g_cache_features[lvl], index, 4);
    const uint16_t floor = W6GetBits(g_cache_floor[lvl], index, 1);
    return floor != 0 || feature == 14;
}

static bool W6TopIsVisible(int lvl, int absX, int absY)
{
    int q = 0, qx = 0, qy = 0;
    if (!W6CAbsToQuadrant(lvl, absX, absY, q, qx, qy)) return false;
    const int index = q * 64 + qy * 8 + qx;
    return W6GetBits(g_cache_hwalls[lvl], index, 2) < 2;
}

static bool W6BottomIsVisible(int lvl, int absX, int absY)
{
    return W6TopIsVisible(lvl, absX, absY - 1);
}

static bool W6RightIsVisible(int lvl, int absX, int absY)
{
    int q = 0, qx = 0, qy = 0;
    if (!W6CAbsToQuadrant(lvl, absX, absY, q, qx, qy)) return false;
    const int index = q * 64 + qy * 8 + qx;
    return W6GetBits(g_cache_vwalls[lvl], index, 2) < 2;
}

static bool W6LeftIsVisible(int lvl, int absX, int absY)
{
    return W6RightIsVisible(lvl, absX - 1, absY);
}

static void MarkVisitedDirty()
{
    if (!g_dirty) g_dirtySinceTick = GetTickCount();
    g_dirty = true;
}

static void SetVisitedCell(int lvl, int q, int qx, int qy, uint8_t value)
{
    if (lvl < 0 || lvl >= W6_LEVEL_COUNT || q < 0 || q >= W6_QUADRANT_COUNT || qx < 0 || qx >= 8 || qy < 0 || qy >= 8) return;
    uint8_t& cell = g_visited[lvl][q][qy][qx];
    if (value == 1) {
        if (cell != 1) {
            cell = 1;
            ++g_totalSteps;
            MarkVisitedDirty();
        }
    } else if (value == 2 && cell == 0) {
        cell = 2;
        MarkVisitedDirty();
    }
}

static void UpdateVisitedLikeOriginal(const Pos& p)
{
    if (p.level >= W6_LEVEL_COUNT || p.quad >= W6_QUADRANT_COUNT || p.qx >= 8 || p.qy >= 8) return;
    const int lvl = (int)p.level;
    if (!g_cache_valid[lvl]) return;

    g_currentDark = W6IsHiddenDarkZone(lvl, (int)p.quad, (int)p.qx, (int)p.qy);
    if (!g_currentDark) SetVisitedCell(lvl, (int)p.quad, (int)p.qx, (int)p.qy, 1);
    if (!g_cfg.showVisibleNeighbors) return;

    const int absX = (int)g_cache_qsx[lvl][p.quad] + (int)p.qx;
    const int absY = (int)g_cache_qsy[lvl][p.quad] + (int)p.qy;

    struct Neighbor { int x; int y; bool visible; } neighbors[4] = {
        { absX,     absY - 1, W6BottomIsVisible(lvl, absX, absY) },
        { absX - 1, absY,     W6LeftIsVisible(lvl, absX, absY) },
        { absX + 1, absY,     W6RightIsVisible(lvl, absX, absY) },
        { absX,     absY + 1, W6TopIsVisible(lvl, absX, absY) },
    };

    for (const Neighbor& n : neighbors) {
        if (!n.visible) continue;
        int q = 0, qx = 0, qy = 0;
        if (!W6CAbsToQuadrant(lvl, n.x, n.y, q, qx, qy)) continue;
        if (g_visited[lvl][q][qy][qx] != 0) continue;
        if (W6IsHiddenDarkZone(lvl, q, qx, qy)) continue;
        SetVisitedCell(lvl, q, qx, qy, 2);
    }
}

static bool GetCurrentAbsXY(int& ax, int& ay)
{
    if (!g_posValid || g_pos.level >= W6_LEVEL_COUNT) return false;
    if (g_cache_valid[g_pos.level] && g_pos.quad < W6_QUADRANT_COUNT) {
        ax = (int)g_cache_qsx[g_pos.level][g_pos.quad] + (int)g_pos.qx;
        ay = (int)g_cache_qsy[g_pos.level][g_pos.quad] + (int)g_pos.qy;
        return true;
    }
    ax = (int)g_pos.absX;
    ay = (int)g_pos.absY;
    return true;
}


static void SaveVisited()
{
    ScopedStateLock lock;
    CreateDirectoryW(CONFIG_DIRECTORY, nullptr);
    FILE* f = _wfopen(VISITED_PATH, L"wb");
    if (!f) return;
    const char magic[8] = { 'W','6','T','R','A','C','E','1' };
    fwrite(magic, 1, 8, f);
    fwrite(g_visited, 1, sizeof(g_visited), f);
    fclose(f);
    g_dirty = false;
    g_dirtySinceTick = 0;
}

static void LoadVisited()
{
    ScopedStateLock lock;
    CreateDirectoryW(CONFIG_DIRECTORY, nullptr);
    if (GetFileAttributesW(VISITED_PATH) == INVALID_FILE_ATTRIBUTES) {
        CopyFileW(L"Config\\W6TraceAutomap_visited.bin", VISITED_PATH, TRUE);
    }
    FILE* f = _wfopen(VISITED_PATH, L"rb");
    if (!f) return;
    char magic[8] = {};
    if (fread(magic, 1, 8, f) == 8 && memcmp(magic, "W6TRACE1", 8) == 0) {
        const size_t got = fread(g_visited, 1, sizeof(g_visited), f);
        if (got == sizeof(g_visited)) {
            g_totalSteps = 0;
            for (int l = 0; l < W6_LEVEL_COUNT; ++l)
                for (int q = 0; q < W6_QUADRANT_COUNT; ++q)
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 8; ++x)
                            if (g_visited[l][q][y][x] == 1) ++g_totalSteps;
            g_dirty = false;
            g_dirtySinceTick = 0;
        }
    }
    fclose(f);
}


static bool WideToUtf8(const std::wstring& text, std::string& utf8)
{
    utf8.clear();
    if (text.empty()) return true;
    if (text.size() > static_cast<size_t>(INT_MAX)) return false;
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return false;
    utf8.resize(static_cast<size_t>(required));
    return WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        utf8.data(), required, nullptr, nullptr) == required;
}

static bool Utf8BytesToWide(const char* data, size_t bytes, std::wstring& text)
{
    text.clear();
    if (bytes == 0) return true;
    if (bytes > static_cast<size_t>(INT_MAX)) return false;
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(bytes), nullptr, 0);
    if (required <= 0) return false;
    text.resize(static_cast<size_t>(required));
    return MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(bytes),
        text.data(), required) == required;
}

static size_t FindNoteIndex(int level, int quadrant, int qx, int qy)
{
    for (size_t i = 0; i < g_notes.size(); ++i) {
        const MapNote& note = g_notes[i];
        if (note.level == level && note.quadrant == quadrant &&
            note.qx == qx && note.qy == qy) return i;
    }
    return static_cast<size_t>(-1);
}

static bool WriteAll(FILE* file, const void* data, size_t bytes)
{
    return bytes == 0 || fwrite(data, 1, bytes, file) == bytes;
}

static bool SaveNotes()
{
    std::vector<MapNote> notes;
    {
        ScopedStateLock lock;
        if (!g_notesDirty) return true;
        notes = g_notes;
    }

    CreateDirectoryW(CONFIG_DIRECTORY, nullptr);
    FILE* file = _wfopen(NOTES_TEMP_PATH, L"wb");
    if (!file) {
        return false;
    }

    static const char magic[8] = { 'W','6','N','O','T','E','2','\0' };
    const uint32_t count = static_cast<uint32_t>(notes.size());
    bool ok = WriteAll(file, magic, sizeof(magic)) &&
              WriteAll(file, &count, sizeof(count));

    for (const MapNote& note : notes) {
        if (!ok) break;
        std::string utf8;
        if (!WideToUtf8(note.text, utf8) || utf8.size() > 65535u) {
            ok = false;
            break;
        }
        const uint8_t coords[4] = { note.level, note.quadrant, note.qx, note.qy };
        const uint32_t color = static_cast<uint32_t>(note.color & 0x00FFFFFFu);
        const uint32_t textBytes = static_cast<uint32_t>(utf8.size());
        ok = WriteAll(file, coords, sizeof(coords)) &&
             WriteAll(file, &color, sizeof(color)) &&
             WriteAll(file, &textBytes, sizeof(textBytes)) &&
             WriteAll(file, utf8.data(), utf8.size());
    }

    if (ok) ok = fflush(file) == 0;
    fclose(file);
    if (!ok || !MoveFileExW(NOTES_TEMP_PATH, NOTES_PATH,
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(NOTES_TEMP_PATH);
        return false;
    }

    {
        ScopedStateLock lock;

        g_notesDirty = false;
    }
    return true;
}

static void LoadNotes()
{
    CreateDirectoryW(CONFIG_DIRECTORY, nullptr);
    FILE* file = _wfopen(NOTES_PATH, L"rb");
    if (!file) return;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    const long fileSize = ftell(file);
    if (fileSize < 12 || fileSize > 8 * 1024 * 1024 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
    const bool readOk = fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    fclose(file);
    if (!readOk) return;
    const bool notesV1 = memcmp(bytes.data(), "W6NOTE1\0", 8) == 0;
    const bool notesV2 = memcmp(bytes.data(), "W6NOTE2\0", 8) == 0;
    if (!notesV1 && !notesV2) return;

    size_t cursor = 8;
    uint32_t count = 0;
    memcpy(&count, bytes.data() + cursor, sizeof(count));
    cursor += sizeof(count);
    const uint32_t maxNoteCount =
        W6_LEVEL_COUNT * W6_QUADRANT_COUNT * W6_TILES_PER_QUADRANT;
    if (count > maxNoteCount) {
        return;
    }

    std::vector<MapNote> loaded;
    loaded.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t fixedBytes = notesV2 ? 12u : 8u;
        if (cursor + fixedBytes > bytes.size()) {
            return;
        }
        MapNote note;
        note.level = bytes[cursor++];
        note.quadrant = bytes[cursor++];
        note.qx = bytes[cursor++];
        note.qy = bytes[cursor++];
        if (notesV2) {
            uint32_t color = 0;
            memcpy(&color, bytes.data() + cursor, sizeof(color));
            cursor += sizeof(color);
            note.color = static_cast<COLORREF>(color & 0x00FFFFFFu);
        }
        uint32_t textBytes = 0;
        memcpy(&textBytes, bytes.data() + cursor, sizeof(textBytes));
        cursor += sizeof(textBytes);
        if (note.level >= W6_LEVEL_COUNT || note.quadrant >= W6_QUADRANT_COUNT ||
            note.qx >= 8 || note.qy >= 8 || textBytes > 65535u ||
            cursor + textBytes > bytes.size() ||
            !Utf8BytesToWide(reinterpret_cast<const char*>(bytes.data() + cursor),
                             textBytes, note.text) ||
            note.text.empty() || note.text.size() > 1024u) {
            return;
        }
        cursor += textBytes;

        bool replaced = false;
        for (MapNote& existing : loaded) {
            if (existing.level == note.level && existing.quadrant == note.quadrant &&
                existing.qx == note.qx && existing.qy == note.qy) {
                existing = std::move(note);
                replaced = true;
                break;
            }
        }
        if (!replaced) loaded.push_back(std::move(note));
    }
    if (cursor != bytes.size()) {
        return;
    }

    {
        ScopedStateLock lock;
        g_notes = std::move(loaded);
        g_notesDirty = false;
    }
}

static void Detach()
{
    HANDLE oldProc = nullptr;
    {
        ScopedStateLock lock;
        oldProc = g_proc;
        g_proc = nullptr;
        g_pid = 0;
        g_attached = false;
        g_dataSeg = 0;
        g_posValid = false;
        g_inGame = false;
        g_currentDark = false;
        g_invalidCount = 0;
        g_cacheMissCount = 0;
        g_autoReconnectRequested = false;
        g_mapScrollX = 0;
        g_mapScrollY = 0;
        g_linkViewActive = false;
        g_viewRecenterPending = true;
        g_learning = false;
        g_wrootCandidates.clear();
        g_lastAnchorProbeTick = 0;
        g_lastAnchorFullScanTick = 0;
        g_activeProcessName.clear();
        g_status = L"Waiting for " + GetWaitingProcessLabel();
    }
    if (oldProc) CloseHandle(oldProc);
    if (g_hwnd) {
        SetWindowTextW(g_hwnd, APP_TITLE);
        InvalidateRect(g_hwnd, nullptr, TRUE);
    }
}

static void AttachOrScan()
{
    if (!g_cfg.enable) {
        ScopedStateLock lock;
        g_status = L"Automap is disabled in Config\\Wizardry6Automap.conf";
        return;
    }
    if (g_proc && !ProcessAlive()) Detach();
    if (!g_proc) {
        DWORD pid = 0;
        std::wstring matchedName;
        if (!FindProcess(pid, matchedName)) {
            ScopedStateLock lock;
            g_status = L"Waiting for " + GetWaitingProcessLabel();
            return;
        }
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE,
                               FALSE, pid);
        if (!h) {
            ScopedStateLock lock;
            g_status = matchedName + L" found, but OpenProcess failed. Run as administrator.";
            return;
        }
        {
            ScopedStateLock lock;
            g_proc = h;
            g_pid = pid;
            g_activeProcessName = matchedName;
            g_attached = false;
            g_dataSeg = 0;
            g_wrootCandidates.clear();
            g_lastAnchorFullScanTick = 0;
            g_status = g_activeProcessName + L" found - waiting for WROOT/WMAZE RAM";
        }
    }

    if (g_attached) return;

    if (g_cfg.manualDataSeg != 0) {
        Pos p;
        if (ReadPosRawAt(g_cfg.manualDataSeg, p)) {
            ScopedStateLock lock;
            g_dataSeg = g_cfg.manualDataSeg;
            g_pos = p;
            g_posValid = ValidatePos(p);
            g_inGame = ValidatePos(p);
            g_attached = true;
            g_learning = false;
            g_status = L"Manual dataseg attached; asynchronous snapshot polling active";
        } else {
            ScopedStateLock lock;
            g_status = L"Manual dataseg is invalid";
        }
        return;
    }

    uintptr_t ds = 0;
    Pos p;
    if (ScanForWrootRamAnchor(ds, p)) {
        {
            ScopedStateLock lock;
            g_dataSeg = ds;
            g_pos = p;
            g_posValid = ValidatePos(p);
            g_inGame = ValidatePos(p);
            g_attached = true;
            g_learning = false;
            g_status = IsStateOk(p.state)
                ? L"Attached by WROOT/WMAZE anchor; 64 KiB asynchronous polling active"
                : L"RAM attached; waiting for a Wizardry 6 maze state";
        }
    } else {
        ScopedStateLock lock;
        g_dataSeg = 0;
        g_posValid = false;
        g_inGame = false;
        g_learning = false;
        g_status = (g_activeProcessName.empty() ? GetWaitingProcessLabel() : g_activeProcessName) +
                   L" found - waiting for WROOT/WMAZE initialization";
    }
}

static void QueueSnapshotMessage()
{
    if (g_hwnd && InterlockedCompareExchange(&g_snapshotMessagePending, 1, 0) == 0) {
        PostMessageW(g_hwnd, WM_APP_SNAPSHOT, 0, 0);
    }
}

static void PollPosition()
{
    bool notify = false;
    {
        ScopedStateLock lock;
        if (!g_cfg.enable || !g_attached || g_learning || !g_proc || g_dataSeg == 0) return;

        static thread_local std::vector<uint8_t> image;
        if (image.size() != g_cfg.snapshotBytes) image.resize(g_cfg.snapshotBytes);

        SIZE_T got = 0;
        if (!ReadProcessMemory(g_proc, (LPCVOID)g_dataSeg, image.data(), image.size(), &got) || got != image.size()) {
            ++g_invalidCount;
            g_status = L"64 KiB snapshot read failed; retaining the last completed map";
            if (g_invalidCount > 120) {
                g_autoReconnectRequested = true;
                g_status = L"Snapshot reads failed repeatedly; automatic reconnect requested";
            }
            notify = true;
        } else {
            Pos p;
            if (!ReadPosFromImage(image.data(), image.size(), p)) {
                ++g_invalidCount;
                g_status = L"Snapshot was read, but configured offsets are outside the read window";
                if (g_invalidCount > 120) g_autoReconnectRequested = true;
                notify = true;
            } else {
                const Pos previousPos = g_pos;
                const bool previousPosValid = g_posValid;
                g_pos = p;
                const bool inGame = IsStateOk(p.state) && p.level < W6_LEVEL_COUNT;
                if (!inGame) {
                    // Match the original Wizardry 6 Automap Mod: stay attached outside the maze,
                    // but do not draw or alter visited state.
                    g_inGame = false;
                    g_posValid = false;
                    g_currentDark = false;
                    g_invalidCount = 0;
                    g_cacheMissCount = 0;
                    g_status = L"Attached; waiting for a Wizardry 6 maze state";
                    notify = true;
                } else if (p.dir >= 4 || p.quad >= W6_QUADRANT_COUNT || p.qx >= 8 || p.qy >= 8) {
                    ++g_invalidCount;
                    g_inGame = true;
                    g_posValid = false;
                    g_status = L"In-game state found, but position fields are temporarily invalid";
                    if (g_invalidCount > 120) g_autoReconnectRequested = true;
                    notify = true;
                } else {
                    DecodedLevelSnapshot decoded;
                    if (!DecodeMapFromImage(image.data(), image.size(), p, decoded)) {
                        ++g_cacheMissCount;
                        g_inGame = true;
                        g_posValid = false;
                        g_status = L"MAP DATA is outside the 64 KiB snapshot or temporarily invalid";
                        if (g_cacheMissCount > 240) g_autoReconnectRequested = true;
                        notify = true;
                    } else {
                        const bool playerMovedOrTurned = !previousPosValid ||
                            previousPos.level != p.level ||
                            previousPos.quad != p.quad ||
                            previousPos.qx != p.qx ||
                            previousPos.qy != p.qy ||
                            previousPos.dir != p.dir;
                        if (playerMovedOrTurned) {
                            // Match the original Wizardry 6 Automap Mod: any step, level change,
                            // or turn immediately returns the player to the center.
                            g_mapScrollX = 0;
                            g_mapScrollY = 0;
                            g_linkViewActive = false;
                            g_viewRecenterPending = true;
                        }

                        PublishDecodedLevel(p, decoded);
                        g_invalidCount = 0;
                        g_cacheMissCount = 0;
                        g_inGame = true;
                        g_posValid = true;
                        UpdateVisitedLikeOriginal(p);
                        g_status = g_currentDark
                            ? L"Dark zone: map hidden exactly like the original Wizardry 6 Automap Mod"
                            : L"Attached: one 64 KiB RAM snapshot every polling cycle";

                        notify = true;
                    }
                }
            }
        }
    }
    if (notify) QueueSnapshotMessage();
}

static DWORD WINAPI PollThreadProc(LPVOID)
{
    for (;;) {
        const DWORD wait = WaitForSingleObject(g_pollStopEvent, (DWORD)g_cfg.pollIntervalMs);
        if (wait != WAIT_TIMEOUT) break;
        PollPosition();
    }
    return 0;
}

static bool StartPollingThread()
{
    if (g_pollThread) return true;
    g_pollStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_pollStopEvent) return false;
    g_pollThread = CreateThread(nullptr, 0, PollThreadProc, nullptr, 0, nullptr);
    if (!g_pollThread) {
        CloseHandle(g_pollStopEvent);
        g_pollStopEvent = nullptr;
        return false;
    }
    return true;
}

static void StopPollingThread()
{
    if (g_pollStopEvent) SetEvent(g_pollStopEvent);
    if (g_pollThread) {
        WaitForSingleObject(g_pollThread, INFINITE);
        CloseHandle(g_pollThread);
        g_pollThread = nullptr;
    }
    if (g_pollStopEvent) {
        CloseHandle(g_pollStopEvent);
        g_pollStopEvent = nullptr;
    }
}

static void FillRectColor(HDC hdc, const RECT& r, COLORREF color)
{
    HBRUSH b = CreateSolidBrush(color);
    FillRect(hdc, &r, b);
    DeleteObject(b);
}

static void DrawArrow(HDC hdc, int cx, int cy, int size, int dir)
{
    if (dir < 0 || dir > 3) return;
    POINT v = g_cfg.dirVec[dir];
    POINT pts[3];
    int len = std::max(6, size / 2);
    int wide = std::max(4, size / 4);
    int tx = cx + v.x * len;
    int ty = cy + v.y * len;
    int px = -v.y;
    int py = v.x;
    pts[0] = { tx, ty };
    pts[1] = { cx - v.x * (len/2) + px * wide, cy - v.y * (len/2) + py * wide };
    pts[2] = { cx - v.x * (len/2) - px * wide, cy - v.y * (len/2) - py * wide };
    HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0,0,0));
    HGDIOBJ oldB = SelectObject(hdc, b);
    HGDIOBJ oldP = SelectObject(hdc, pen);
    Polygon(hdc, pts, 3);
    SelectObject(hdc, oldP);
    SelectObject(hdc, oldB);
    DeleteObject(pen);
    DeleteObject(b);
}


static int W6TileVisibilityKind(int lvl, int q, int y, int x)
{
    if (lvl < 0 || lvl >= W6_LEVEL_COUNT || q < 0 || q >= W6_QUADRANT_COUNT || y < 0 || y >= 8 || x < 0 || x >= 8) return 0;
    const int v = (int)g_visited[lvl][q][y][x];
    if (v != 0) return v;
    return g_cfg.revealAll ? 2 : 0;
}

static int W6VisibilityAtAbs(int lvl, int absX, int absY)
{
    int q = 0, qx = 0, qy = 0;
    if (!W6CAbsToQuadrant(lvl, absX, absY, q, qx, qy)) return 0;
    return W6TileVisibilityKind(lvl, q, qy, qx);
}

struct MapPens {
    HPEN passage = nullptr;
    HPEN passageDim = nullptr;
    HPEN wall = nullptr;
    HPEN wallDim = nullptr;
    HPEN door = nullptr;
    HPEN doorDim = nullptr;
    HPEN port = nullptr;
    HPEN portDim = nullptr;
};

static void DrawPortcullisEdge(HDC hdc, int x1, int y1, int x2, int y2, bool horizontal, HPEN pen)
{
    HGDIOBJ old = SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y1, nullptr);
    LineTo(hdc, x2, y2);
    const int length = horizontal ? abs(x2 - x1) : abs(y2 - y1);
    const int bars = std::max(2, length / 6);
    for (int i = 1; i < bars; ++i) {
        if (horizontal) {
            const int x = x1 + (x2 - x1) * i / bars;
            MoveToEx(hdc, x, y1 - 2, nullptr);
            LineTo(hdc, x, y1 + 3);
        } else {
            const int y = y1 + (y2 - y1) * i / bars;
            MoveToEx(hdc, x1 - 2, y, nullptr);
            LineTo(hdc, x1 + 3, y);
        }
    }
    SelectObject(hdc, old);
}

static void DrawMapEdge(HDC hdc, int x1, int y1, int x2, int y2, bool horizontal,
                        uint16_t wall, bool portcullis, bool dim, const MapPens& pens)
{
    if (wall == 0) return;
    if (portcullis && wall == 2) {
        DrawPortcullisEdge(hdc, x1, y1, x2, y2, horizontal, dim ? pens.portDim : pens.port);
        return;
    }
    HPEN pen = nullptr;
    if (wall == 1) pen = dim ? pens.passageDim : pens.passage;
    else if (wall == 2) pen = dim ? pens.wallDim : pens.wall;
    else if (wall == 3) pen = dim ? pens.doorDim : pens.door;
    if (!pen) return;
    HGDIOBJ old = SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y1, nullptr);
    LineTo(hdc, x2, y2);
    SelectObject(hdc, old);
}

static void DrawStairsIcon(HDC hdc, int sx, int sy, int cell, uint16_t feature, uint16_t dir, bool dim)
{
    const COLORREF color = dim ? RGB(115,115,125) : RGB(225,225,235);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old = SelectObject(hdc, pen);
    const int left = sx + std::max(2, cell / 5);
    const int right = sx + cell - std::max(2, cell / 5);
    const int top = sy + std::max(2, cell / 5);
    const int step = std::max(2, cell / 6);
    for (int i = 0; i < 3; ++i) {
        MoveToEx(hdc, left + i * step / 2, top + i * step, nullptr);
        LineTo(hdc, right - i * step / 2, top + i * step);
    }
    POINT v = g_cfg.dirVec[dir & 3];
    const int cx = sx + cell / 2;
    const int cy = sy + cell / 2;
    MoveToEx(hdc, cx, cy, nullptr);
    LineTo(hdc, cx + v.x * std::max(3, cell / 3), cy + v.y * std::max(3, cell / 3));
    SelectObject(hdc, old);
    DeleteObject(pen);

    wchar_t label[2] = { feature == 1 ? L'U' : L'D', 0 };
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    TextOutW(hdc, sx + 2, sy + 1, label, 1);
}

static void DrawFountainIcon(HDC hdc, int sx, int sy, int cell, bool dim)
{
    const COLORREF color = dim ? RGB(55,95,125) : RGB(80,180,235);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldP = SelectObject(hdc, pen);
    HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    const int m = std::max(3, cell / 4);
    Ellipse(hdc, sx + m, sy + m, sx + cell - m + 1, sy + cell - m + 1);
    MoveToEx(hdc, sx + cell / 2, sy + m - 1, nullptr);
    LineTo(hdc, sx + cell / 2, sy + cell - m + 2);
    MoveToEx(hdc, sx + m - 1, sy + cell / 2, nullptr);
    LineTo(hdc, sx + cell - m + 2, sy + cell / 2);
    SelectObject(hdc, oldB);
    SelectObject(hdc, oldP);
    DeleteObject(pen);
}

static bool GetMapViewLocked(int& level, int& anchorAbsX, int& anchorAbsY)
{
    if (!g_inGame || !g_posValid || g_pos.level >= W6_LEVEL_COUNT) return false;
    if (g_linkViewActive) {
        if (g_linkViewLevel < 0 || g_linkViewLevel >= W6_LEVEL_COUNT ||
            !g_cache_valid[g_linkViewLevel]) return false;
        level = g_linkViewLevel;
        anchorAbsX = g_linkViewAbsX;
        anchorAbsY = g_linkViewAbsY;
        return true;
    }

    level = static_cast<int>(g_pos.level);
    if (!g_cache_valid[level]) return false;
    return GetCurrentAbsXY(anchorAbsX, anchorAbsY);
}

static bool HitTestMapTileLocked(HWND hwnd, int mouseX, int mouseY,
                                 int& level, int& quadrant, int& qx, int& qy)
{
    if (g_cfg.hideInDarkZones && g_currentDark) return false;

    int currentLevel = 0;
    int currentAbsX = 0;
    int currentAbsY = 0;
    if (!GetMapViewLocked(currentLevel, currentAbsX, currentAbsY)) return false;

    RECT client = {};
    if (!GetClientRect(hwnd, &client)) return false;
    const int width = static_cast<int>(client.right - client.left);
    const int height = static_cast<int>(client.bottom - client.top);
    const int top = 8;
    const int bottom = height - 8;
    const int availableWidth = std::max(100, width - 36);
    const int availableHeight = std::max(100, bottom - top - 8);
    const int baseCell = std::min(24, std::max(10,
        std::min(availableWidth / 42, availableHeight / 30)));
    const int cell = std::min(48, baseCell * 2);
    const int centerX = width / 2 - cell / 2;
    const int centerY = top + availableHeight / 2 - cell / 2;

    for (int q = 0; q < W6_QUADRANT_COUNT; ++q) {
        const int startX = g_cache_qsx[currentLevel][q];
        const int startY = g_cache_qsy[currentLevel][q];
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const int sx = centerX + (startX + x - currentAbsX) * cell + g_mapScrollX;
                const int deltaY = (startY + y - currentAbsY) * cell;
                const int sy = centerY + (g_cfg.invertY ? -deltaY : deltaY) + g_mapScrollY;
                if (mouseX >= sx && mouseX < sx + cell &&
                    mouseY >= sy && mouseY < sy + cell) {
                    level = currentLevel;
                    quadrant = q;
                    qx = x;
                    qy = y;
                    return true;
                }
            }
        }
    }
    return false;
}

static void DrawSingleDecodedMap(HDC hdc, int W, int top, int bottom)
{
    if (g_cfg.hideInDarkZones && g_currentDark) return;

    int lvl = 0;
    int curAbsX = 0;
    int curAbsY = 0;
    if (!GetMapViewLocked(lvl, curAbsX, curAbsY)) return;

    int availW = std::max(100, W - 36);
    int availH = std::max(100, bottom - top - 8);
    // Beta12's adaptive size was about 11 px in a 512 x 512 client area.
    // Double it so the normal 512 x 512 view matches the old automap's
    // 22-pixel square size while retaining adaptive scaling for other sizes.
    const int baseCell = std::min(24, std::max(10, std::min(availW / 42, availH / 30)));
    const int cell = std::min(48, baseCell * 2);
    int centerX = W / 2 - cell / 2;
    int centerY = top + availH / 2 - cell / 2;

    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(34,34,38));
    HPEN currentPen = CreatePen(PS_SOLID, 2, RGB(255,230,70));
    MapPens pens;
    pens.passage    = CreatePen(PS_SOLID, 1, RGB(100,100,112));
    pens.passageDim = CreatePen(PS_SOLID, 1, RGB(55,55,64));
    pens.wall       = CreatePen(PS_SOLID, 2, RGB(205,205,210));
    pens.wallDim    = CreatePen(PS_SOLID, 2, RGB(95,95,102));
    pens.door       = CreatePen(PS_SOLID, 2, RGB(255,180,0));
    pens.doorDim    = CreatePen(PS_SOLID, 2, RGB(145,90,0));
    pens.port       = CreatePen(PS_SOLID, 1, RGB(150,200,205));
    pens.portDim    = CreatePen(PS_SOLID, 1, RGB(70,95,98));
    HGDIOBJ oldPen = SelectObject(hdc, gridPen);

    auto screenXY = [&](int absX, int absY, int& sx, int& sy) {
        sx = centerX + (absX - curAbsX) * cell + g_mapScrollX;
        const int dy = (absY - curAbsY) * cell;
        sy = centerY + (g_cfg.invertY ? -dy : dy) + g_mapScrollY;
    };

    // First pass: floor/light/water state. Value 2 is deliberately dimmed,
    // matching the original visdata behavior for merely visible neighbors.
    for (int q = 0; q < W6_QUADRANT_COUNT; ++q) {
        const int qsx = g_cache_qsx[lvl][q];
        const int qsy = g_cache_qsy[lvl][q];
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const int vis = W6TileVisibilityKind(lvl, q, y, x);
                if (vis == 0) continue;
                const int absX = qsx + x;
                const int absY = qsy + y;
                int sx = 0, sy = 0;
                screenXY(absX, absY, sx, sy);
                if (sx + cell < 0 || sx > W || sy + cell < top || sy > bottom) continue;

                const int idx = q * 64 + y * 8 + x;
                const uint16_t feature = W6GetBits(g_cache_features[lvl], idx, 4);
                const uint16_t floor = W6GetBits(g_cache_floor[lvl], idx, 1);
                const uint16_t roof = W6GetBits(g_cache_roof[lvl], idx, 1);
                const bool dim = vis != 1;
                const bool water = (lvl == 8 || lvl == 10 || lvl == 12) && floor != 0 && (roof != 0 || lvl == 8);

                COLORREF fill;
                if (water) fill = dim ? RGB(20,42,58) : RGB(28,82,116);
                else if (floor == 0 && feature != 14) fill = dim ? RGB(18,19,23) : RGB(36,38,44);
                else if (feature == 14) fill = dim ? RGB(24,17,18) : RGB(48,25,27);
                else fill = dim ? RGB(26,28,31) : RGB(50,54,58);

                RECT r = { sx + 1, sy + 1, sx + cell, sy + cell };
                FillRectColor(hdc, r, fill);
                SelectObject(hdc, gridPen);
                SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, sx, sy, sx + cell, sy + cell);
            }
        }
    }

    // Second pass: top and right encoded edges. Draw an edge when either side
    // is known, as the original automap did. Add solid outer left/bottom edges.
    for (int q = 0; q < W6_QUADRANT_COUNT; ++q) {
        const int qsx = g_cache_qsx[lvl][q];
        const int qsy = g_cache_qsy[lvl][q];
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const int absX = qsx + x;
                const int absY = qsy + y;
                const int currentVis = W6TileVisibilityKind(lvl, q, y, x);
                const int rightVis = W6VisibilityAtAbs(lvl, absX + 1, absY);
                const int topVis = W6VisibilityAtAbs(lvl, absX, absY + 1);
                if (currentVis == 0 && rightVis == 0 && topVis == 0) continue;

                int sx = 0, sy = 0;
                screenXY(absX, absY, sx, sy);
                if (sx + cell < -2 || sx > W + 2 || sy + cell < top - 2 || sy > bottom + 2) continue;

                const int idx = q * 64 + y * 8 + x;
                const uint16_t feature = W6GetBits(g_cache_features[lvl], idx, 4);
                const uint16_t featureDir = W6GetBits(g_cache_features_dirs[lvl], idx, 2);
                const uint16_t hw = W6GetBits(g_cache_hwalls[lvl], idx, 2);
                const uint16_t vw = W6GetBits(g_cache_vwalls[lvl], idx, 2);

                if (currentVis != 0 || rightVis != 0) {
                    const bool dim = currentVis != 1 && rightVis != 1;
                    const bool port = feature == 7 && featureDir == 1;
                    DrawMapEdge(hdc, sx + cell, sy, sx + cell, sy + cell + 1, false, vw, port, dim, pens);
                }
                if (currentVis != 0 || topVis != 0) {
                    const bool dim = currentVis != 1 && topVis != 1;
                    const bool port = feature == 7 && featureDir == 0;
                    DrawMapEdge(hdc, sx, sy, sx + cell + 1, sy, true, hw, port, dim, pens);
                }

                if (currentVis != 0 && W6VisibilityAtAbs(lvl, absX - 1, absY) == 0) {
                    int nq = 0, nx = 0, ny = 0;
                    if (!W6CAbsToQuadrant(lvl, absX - 1, absY, nq, nx, ny)) {
                        DrawMapEdge(hdc, sx, sy, sx, sy + cell + 1, false, 2, false, currentVis != 1, pens);
                    }
                }
                if (currentVis != 0 && W6VisibilityAtAbs(lvl, absX, absY - 1) == 0) {
                    int nq = 0, nx = 0, ny = 0;
                    if (!W6CAbsToQuadrant(lvl, absX, absY - 1, nq, nx, ny)) {
                        DrawMapEdge(hdc, sx, sy + cell, sx + cell + 1, sy + cell, true, 2, false, currentVis != 1, pens);
                    }
                }
            }
        }
    }

    // Third pass: original feature usage: stairs, fountain and portcullis.
    for (int q = 0; q < W6_QUADRANT_COUNT; ++q) {
        const int qsx = g_cache_qsx[lvl][q];
        const int qsy = g_cache_qsy[lvl][q];
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const int vis = W6TileVisibilityKind(lvl, q, y, x);
                if (vis == 0) continue;
                const int idx = q * 64 + y * 8 + x;
                const uint16_t feature = W6GetBits(g_cache_features[lvl], idx, 4);
                const uint16_t featureDir = W6GetBits(g_cache_features_dirs[lvl], idx, 2);
                if (feature != 1 && feature != 2 && feature != 4) continue;
                int sx = 0, sy = 0;
                screenXY(qsx + x, qsy + y, sx, sy);
                if (feature == 1 || feature == 2) DrawStairsIcon(hdc, sx, sy, cell, feature, featureDir, vis != 1);
                else if (feature == 4) DrawFountainIcon(hdc, sx, sy, cell, vis != 1);
            }
        }
    }

    HGDIOBJ previousNoteBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    for (const MapNote& note : g_notes) {
        if (note.level != lvl || note.quadrant >= W6_QUADRANT_COUNT ||
            note.qx >= 8 || note.qy >= 8) continue;
        int sx = 0;
        int sy = 0;
        screenXY(static_cast<int>(g_cache_qsx[lvl][note.quadrant]) + note.qx,
                 static_cast<int>(g_cache_qsy[lvl][note.quadrant]) + note.qy,
                 sx, sy);
        if (sx + cell < 0 || sx > W || sy + cell < top || sy > bottom) continue;
        HPEN notePen = CreatePen(PS_SOLID, 3, note.color);
        if (!notePen) continue;
        HGDIOBJ previousNotePen = SelectObject(hdc, notePen);
        const int inset = std::max(2, cell / 8);
        Rectangle(hdc, sx + inset, sy + inset,
                  sx + cell - inset + 1, sy + cell - inset + 1);
        SelectObject(hdc, previousNotePen);
        DeleteObject(notePen);
    }
    SelectObject(hdc, previousNoteBrush);

    if (g_pos.level == lvl &&
        g_pos.quad < W6_QUADRANT_COUNT && g_pos.qx < 8 && g_pos.qy < 8) {
        const int absX = (int)g_cache_qsx[lvl][g_pos.quad] + (int)g_pos.qx;
        const int absY = (int)g_cache_qsy[lvl][g_pos.quad] + (int)g_pos.qy;
        int sx = 0, sy = 0;
        screenXY(absX, absY, sx, sy);
        SelectObject(hdc, currentPen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, sx - 2, sy - 2, sx + cell + 2, sy + cell + 2);
        DrawArrow(hdc, sx + cell / 2, sy + cell / 2, cell, g_pos.dir);
    }

    SelectObject(hdc, oldPen);
    DeleteObject(gridPen);
    DeleteObject(currentPen);
    DeleteObject(pens.passage);
    DeleteObject(pens.passageDim);
    DeleteObject(pens.wall);
    DeleteObject(pens.wallDim);
    DeleteObject(pens.door);
    DeleteObject(pens.doorDim);
    DeleteObject(pens.port);
    DeleteObject(pens.portDim);
}

static void DrawCenteredStatus(HDC hdc, int W, int H, const wchar_t* text)
{
    RECT r = { 20, 20, W - 20, H - 20 };
    SetTextColor(hdc, RGB(155,155,165));
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}


static void Paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc0 = BeginPaint(hwnd, &ps);
    ScopedStateLock stateLock;
    RECT client;
    GetClientRect(hwnd, &client);
    const int W = client.right - client.left;
    const int H = client.bottom - client.top;
    HDC hdc = CreateCompatibleDC(hdc0);
    HBITMAP bmp = CreateCompatibleBitmap(hdc0, W, H);
    HGDIOBJ oldBmp = SelectObject(hdc, bmp);

    RECT bg = {0,0,W,H};
    FillRectColor(hdc, bg, RGB(8,8,10));

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ oldFont = SelectObject(hdc, font);

    const bool canDraw = g_cfg.enable && g_inGame && g_posValid &&
                         g_pos.level < W6_LEVEL_COUNT &&
                         g_cache_valid[g_pos.level] &&
                         !(g_cfg.hideInDarkZones && g_currentDark);
    if (canDraw) {
        DrawSingleDecodedMap(hdc, W, 8, H - 8);
    } else if (!g_cfg.enable) {
        DrawCenteredStatus(hdc, W, H, L"Automap is disabled");
    } else if (g_cfg.hideInDarkZones && g_currentDark) {
        // Original Wizardry 6 Automap Mod behavior: a dark zone shows a completely blank map.
    } else if (g_learning) {
        DrawCenteredStatus(hdc, W, H, L"Move or turn once in Wizardry 6");
    } else if (!g_proc) {
        const std::wstring waitingText = L"Waiting for " + GetWaitingProcessLabel() + L"...";
        DrawCenteredStatus(hdc, W, H, waitingText.c_str());
    } else if (!g_inGame) {
        DrawCenteredStatus(hdc, W, H, L"Waiting for the maze...");
    } else {
        DrawCenteredStatus(hdc, W, H, L"Waiting for map data...");
    }

    SelectObject(hdc, oldFont);
    BitBlt(hdc0, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
    EndPaint(hwnd, &ps);
}

static int MouseCoordX(LPARAM lp)
{
    return static_cast<int>(static_cast<short>(LOWORD(lp)));
}

static int MouseCoordY(LPARAM lp)
{
    return static_cast<int>(static_cast<short>(HIWORD(lp)));
}

static void StopMapDrag(HWND hwnd)
{
    g_draggingMap = false;
    if (GetCapture() == hwnd) ReleaseCapture();
}


static bool SameNoteLocation(const NoteHoverTarget& left,
                             const NoteHoverTarget& right)
{
    return left.level == right.level &&
           left.quadrant == right.quadrant &&
           left.qx == right.qx &&
           left.qy == right.qy;
}

static bool QueryNoteAtPoint(HWND hwnd, int mouseX, int mouseY,
                             NoteHoverTarget& target)
{
    int level = 0;
    int quadrant = 0;
    int qx = 0;
    int qy = 0;

    ScopedStateLock lock;
    if (!HitTestMapTileLocked(hwnd, mouseX, mouseY,
                              level, quadrant, qx, qy)) return false;
    const size_t index = FindNoteIndex(level, quadrant, qx, qy);
    if (index == static_cast<size_t>(-1)) return false;

    target.level = level;
    target.quadrant = quadrant;
    target.qx = qx;
    target.qy = qy;
    target.text = g_notes[index].text;
    return true;
}

static LRESULT CALLBACK NoteTooltipWndProc(HWND hwnd, UINT msg,
                                             WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!hdc) return 0;

        RECT client = {};
        GetClientRect(hwnd, &client);
        FillRect(hdc, &client, GetSysColorBrush(COLOR_INFOBK));

        HPEN borderPen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_WINDOWFRAME));
        HGDIOBJ oldPen = borderPen ? SelectObject(hdc, borderPen) : nullptr;
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, client.left, client.top, client.right, client.bottom);
        SelectObject(hdc, oldBrush);
        if (borderPen) {
            SelectObject(hdc, oldPen);
            DeleteObject(borderPen);
        }

        const UINT dpi = GetWindowDpiCompat(hwnd);
        const int padX = MulDiv(8, static_cast<int>(dpi), 96);
        const int padY = MulDiv(6, static_cast<int>(dpi), 96);
        RECT textRect = client;
        InflateRect(&textRect, -padX, -padY);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_INFOTEXT));
        HGDIOBJ oldFont = SelectObject(
            hdc, g_noteTooltipFont
                ? reinterpret_cast<HGDIOBJ>(g_noteTooltipFont)
                : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(hdc, g_noteTooltipText.c_str(), -1, &textRect,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(hdc, oldFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool RegisterNoteTooltipClass()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = NoteTooltipWndProc;
    wc.hInstance = reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = NOTE_TOOLTIP_CLASS;

    if (RegisterClassExW(&wc)) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static void RecreateNoteTooltipFont(HWND referenceWindow)
{
    if (g_noteTooltipFont) {
        DeleteObject(g_noteTooltipFont);
        g_noteTooltipFont = nullptr;
    }

    const UINT dpi = GetWindowDpiCompat(referenceWindow);
    const int height = -MulDiv(9, static_cast<int>(dpi), 72);
    g_noteTooltipFont = CreateFontW(
        height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void DeactivateNoteTooltip()
{
    if (g_noteTooltip && g_noteTooltipVisible && IsWindow(g_noteTooltip)) {
        ShowWindow(g_noteTooltip, SW_HIDE);
    }
    g_noteTooltipVisible = false;
}

static void CancelNoteTooltip(HWND hwnd)
{
    (void)hwnd;
    DeactivateNoteTooltip();
    g_noteHoverTarget = NoteHoverTarget{};
    g_noteTooltipText.clear();
}

static bool CreateNoteTooltip(HWND parent)
{
    if (!RegisterNoteTooltipClass()) return false;

    g_noteTooltip = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        NOTE_TOOLTIP_CLASS, nullptr, WS_POPUP,
        0, 0, 0, 0, parent, nullptr,
        reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr)), nullptr);
    if (!g_noteTooltip) return false;

    RecreateNoteTooltipFont(parent);
    return true;
}

static void ShowPendingNoteTooltip(HWND hwnd);

static void BeginNoteHover(HWND hwnd, int mouseX, int mouseY)
{
    NoteHoverTarget target;
    if (!QueryNoteAtPoint(hwnd, mouseX, mouseY, target)) {
        CancelNoteTooltip(hwnd);
        return;
    }

    if (g_noteHoverTarget.IsValid() &&
        SameNoteLocation(g_noteHoverTarget, target)) {
        if (g_noteHoverTarget.text != target.text) {
            CancelNoteTooltip(hwnd);
            g_noteHoverTarget = std::move(target);
            ShowPendingNoteTooltip(hwnd);
        }
        return;
    }

    CancelNoteTooltip(hwnd);
    g_noteHoverTarget = std::move(target);
    ShowPendingNoteTooltip(hwnd);
}

static void ShowPendingNoteTooltip(HWND hwnd)
{
    if (!g_noteTooltip || !g_noteHoverTarget.IsValid() || g_draggingMap) return;

    POINT screenPoint = {};
    if (!GetCursorPos(&screenPoint)) return;
    POINT clientPoint = screenPoint;
    if (!ScreenToClient(hwnd, &clientPoint)) return;

    RECT client = {};
    if (!GetClientRect(hwnd, &client) || !PtInRect(&client, clientPoint)) {
        CancelNoteTooltip(hwnd);
        return;
    }

    NoteHoverTarget current;
    if (!QueryNoteAtPoint(hwnd, clientPoint.x, clientPoint.y, current) ||
        !SameNoteLocation(g_noteHoverTarget, current)) {
        CancelNoteTooltip(hwnd);
        return;
    }

    g_noteHoverTarget = std::move(current);
    g_noteTooltipText = g_noteHoverTarget.text;
    if (g_noteTooltipText.empty()) {
        CancelNoteTooltip(hwnd);
        return;
    }

    const UINT dpi = GetWindowDpiCompat(hwnd);
    const int padX = MulDiv(8, static_cast<int>(dpi), 96);
    const int padY = MulDiv(6, static_cast<int>(dpi), 96);
    const int maxTextWidth = MulDiv(420, static_cast<int>(dpi), 96);

    HDC hdc = GetDC(g_noteTooltip);
    if (!hdc) return;
    HGDIOBJ oldFont = SelectObject(
        hdc, g_noteTooltipFont
            ? reinterpret_cast<HGDIOBJ>(g_noteTooltipFont)
            : GetStockObject(DEFAULT_GUI_FONT));
    RECT measured = { 0, 0, maxTextWidth, 0 };
    DrawTextW(hdc, g_noteTooltipText.c_str(), -1, &measured,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX |
              DT_EDITCONTROL | DT_CALCRECT);
    SelectObject(hdc, oldFont);
    ReleaseDC(g_noteTooltip, hdc);

    const int measuredWidth = std::max(
        0, static_cast<int>(measured.right - measured.left));
    const int measuredHeight = std::max(
        0, static_cast<int>(measured.bottom - measured.top));
    const int width = std::max(
        MulDiv(48, static_cast<int>(dpi), 96),
        std::min(maxTextWidth, measuredWidth) + padX * 2);
    const int height = std::max(
        MulDiv(24, static_cast<int>(dpi), 96),
        measuredHeight + padY * 2);
    const int offsetX = MulDiv(16, static_cast<int>(dpi), 96);
    const int offsetY = MulDiv(22, static_cast<int>(dpi), 96);

    int x = screenPoint.x + offsetX;
    int y = screenPoint.y + offsetY;
    HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        const RECT& work = monitorInfo.rcWork;
        if (x + width > work.right) x = screenPoint.x - width - offsetX;
        if (y + height > work.bottom) y = screenPoint.y - height - offsetY;
        x = std::max(static_cast<int>(work.left),
                     std::min(x, static_cast<int>(work.right) - width));
        y = std::max(static_cast<int>(work.top),
                     std::min(y, static_cast<int>(work.bottom) - height));
    }

    SetWindowPos(g_noteTooltip, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_noteTooltip, nullptr, TRUE);
    UpdateWindow(g_noteTooltip);
    g_noteTooltipVisible = true;
}

struct NoteDialogState {
    HWND edit = nullptr;
    std::wstring initialText;
    std::wstring resultText;
    bool accepted = false;
    bool done = false;
};

static LRESULT CALLBACK NoteDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NoteDialogState* state = reinterpret_cast<NoteDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        state = static_cast<NoteDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        if (!state) return -1;

        const UINT dpi = GetWindowDpiCompat(hwnd);
        auto scaled = [dpi](int value) {
            return MulDiv(value, static_cast<int>(dpi), 96);
        };
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        HWND label = CreateWindowExW(0, L"STATIC",
            L"Enter a note. Leave it blank to delete the marker.",
            WS_CHILD | WS_VISIBLE,
            scaled(12), scaled(12), scaled(416), scaled(20),
            hwnd, nullptr, nullptr, nullptr);
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            state->initialText.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            scaled(12), scaled(38), scaled(416), scaled(25),
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)),
            nullptr, nullptr);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            scaled(268), scaled(78), scaled(76), scaled(26),
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
            nullptr, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            scaled(352), scaled(78), scaled(76), scaled(26),
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
            nullptr, nullptr);
        if (!label || !state->edit || !ok || !cancel) return -1;

        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(state->edit, EM_SETLIMITTEXT, 1024, 0);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);
        SetFocus(state->edit);
        return 0;
    }

    case WM_COMMAND:
        if (!state) return 0;
        if (LOWORD(wp) == IDOK) {
            const int length = GetWindowTextLengthW(state->edit);
            std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1u, L'\0');
            const int copied = GetWindowTextW(state->edit, buffer.data(), length + 1);
            state->resultText.assign(buffer.data(),
                                     static_cast<size_t>(std::max(0, copied)));
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (state) state->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool RegisterNoteDialogClass()
{
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = NoteDialogProc;
    wc.hInstance = reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(
        static_cast<ULONG_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = NOTE_DIALOG_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    registered = true;
    return true;
}

static bool ShowNoteEditor(HWND parent, const std::wstring& initialText,
                           std::wstring& resultText)
{
    if (!RegisterNoteDialogClass()) return false;

    NoteDialogState state;
    state.initialText = initialText;
    const UINT dpi = GetWindowDpiCompat(parent);
    RECT windowRect = { 0, 0, MulDiv(440, static_cast<int>(dpi), 96),
                        MulDiv(118, static_cast<int>(dpi), 96) };
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    if (!AdjustWindowRectForDpiCompat(&windowRect, style, FALSE, 0, dpi)) {
        return false;
    }

    RECT parentRect = {};
    GetWindowRect(parent, &parentRect);
    const int width = static_cast<int>(windowRect.right - windowRect.left);
    const int height = static_cast<int>(windowRect.bottom - windowRect.top);
    const int x = static_cast<int>(parentRect.left) +
                  (static_cast<int>(parentRect.right - parentRect.left) - width) / 2;
    const int y = static_cast<int>(parentRect.top) +
                  (static_cast<int>(parentRect.bottom - parentRect.top) - height) / 2;

    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        NOTE_DIALOG_CLASS, L"Map Note", style,
        x, y, width, height, parent, nullptr,
        reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr)), &state);
    if (!dialog) return false;

    const BOOL parentWasEnabled = IsWindowEnabled(parent);
    EnableWindow(parent, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG msg = {};
    int getMessageResult = 1;
    while (!state.done &&
           (getMessageResult = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (getMessageResult == 0) PostQuitMessage(static_cast<int>(msg.wParam));

    if (parentWasEnabled) EnableWindow(parent, TRUE);
    SetActiveWindow(parent);
    SetFocus(parent);
    if (!state.accepted) return false;
    resultText = std::move(state.resultText);
    return true;
}

static bool ContainsNonWhitespace(const std::wstring& text)
{
    return std::any_of(text.begin(), text.end(),
        [](wchar_t ch) { return std::iswspace(ch) == 0; });
}

static void EditNoteAtPoint(HWND hwnd, int mouseX, int mouseY)
{
    CancelNoteTooltip(hwnd);
    int level = 0;
    int quadrant = 0;
    int qx = 0;
    int qy = 0;
    std::wstring initialText;
    {
        ScopedStateLock lock;
        if (!HitTestMapTileLocked(hwnd, mouseX, mouseY,
                                  level, quadrant, qx, qy)) return;
        const size_t index = FindNoteIndex(level, quadrant, qx, qy);
        if (index != static_cast<size_t>(-1)) initialText = g_notes[index].text;
    }

    std::wstring editedText;
    if (!ShowNoteEditor(hwnd, initialText, editedText)) return;

    bool changed = false;
    {
        ScopedStateLock lock;
        const size_t index = FindNoteIndex(level, quadrant, qx, qy);
        if (ContainsNonWhitespace(editedText)) {
            if (index == static_cast<size_t>(-1)) {
                MapNote note;
                note.level = static_cast<uint8_t>(level);
                note.quadrant = static_cast<uint8_t>(quadrant);
                note.qx = static_cast<uint8_t>(qx);
                note.qy = static_cast<uint8_t>(qy);
                note.text = std::move(editedText);
                g_notes.push_back(std::move(note));
                changed = true;
            } else if (g_notes[index].text != editedText) {
                g_notes[index].text = std::move(editedText);
                changed = true;
            }
        } else if (index != static_cast<size_t>(-1)) {
            g_notes.erase(g_notes.begin() + static_cast<ptrdiff_t>(index));
            changed = true;
        }
        if (changed) g_notesDirty = true;
    }

    if (!changed) return;
    if (!SaveNotes()) {
        MessageBoxW(hwnd,
            L"The map note could not be saved. It will be retried when the automap closes.",
            APP_TITLE, MB_OK | MB_ICONWARNING);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

static bool IsAltDown()
{
    return (GetKeyState(VK_MENU) & 0x8000) != 0;
}

static bool IsCtrlDown()
{
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
}

static bool SetClipboardText(HWND owner, const std::wstring& text)
{
    if (!OpenClipboard(owner)) return false;
    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }

    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);

    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

static void CopyCoordinateAtPoint(HWND hwnd, int mouseX, int mouseY)
{
    int level = 0;
    int quadrant = 0;
    int qx = 0;
    int qy = 0;
    {
        ScopedStateLock lock;
        if (!HitTestMapTileLocked(hwnd, mouseX, mouseY,
                                  level, quadrant, qx, qy)) return;
    }

    wchar_t coordinate[64] = {};
    swprintf(coordinate, sizeof(coordinate) / sizeof(coordinate[0]),
             L"{%d:%d:%d:%d}", level, quadrant, qx, qy);
    SetClipboardText(hwnd, coordinate);
}

static bool ParseCoordinateLink(const std::wstring& text,
                                int& level, int& quadrant, int& qx, int& qy)
{
    const size_t start = text.find(L'{');
    if (start == std::wstring::npos) return false;
    const size_t end = text.find(L'}', start + 1);
    if (end == std::wstring::npos || end <= start) return false;

    const std::wstring link = text.substr(start, end - start + 1);
    if (swscanf(link.c_str(), L"{%d:%d:%d:%d}",
                &level, &quadrant, &qx, &qy) != 4) return false;

    return level >= 0 && level < W6_LEVEL_COUNT &&
           quadrant >= 0 && quadrant < W6_QUADRANT_COUNT &&
           qx >= 0 && qx < 8 && qy >= 0 && qy < 8;
}

static void FollowCoordinateLinkAtPoint(HWND hwnd, int mouseX, int mouseY)
{
    int noteLevel = 0;
    int noteQuadrant = 0;
    int noteQx = 0;
    int noteQy = 0;
    int linkLevel = 0;
    int linkQuadrant = 0;
    int linkQx = 0;
    int linkQy = 0;

    {
        ScopedStateLock lock;
        if (!HitTestMapTileLocked(hwnd, mouseX, mouseY,
                                  noteLevel, noteQuadrant, noteQx, noteQy)) return;
        const size_t noteIndex = FindNoteIndex(noteLevel, noteQuadrant, noteQx, noteQy);
        if (noteIndex == static_cast<size_t>(-1)) return;
        if (!ParseCoordinateLink(g_notes[noteIndex].text,
                                 linkLevel, linkQuadrant, linkQx, linkQy)) return;
        if (!g_cache_valid[linkLevel]) return;

        g_linkViewLevel = linkLevel;
        g_linkViewAbsX = static_cast<int>(g_cache_qsx[linkLevel][linkQuadrant]) + linkQx;
        g_linkViewAbsY = static_cast<int>(g_cache_qsy[linkLevel][linkQuadrant]) + linkQy;
        g_linkViewActive = true;
        g_mapScrollX = 0;
        g_mapScrollY = 0;
    }

    StopMapDrag(hwnd);
    CancelNoteTooltip(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void ChangeNoteColorAtPoint(HWND hwnd, int mouseX, int mouseY)
{
    int level = 0;
    int quadrant = 0;
    int qx = 0;
    int qy = 0;
    COLORREF color = DEFAULT_NOTE_COLOR;
    {
        ScopedStateLock lock;
        if (!HitTestMapTileLocked(hwnd, mouseX, mouseY,
                                  level, quadrant, qx, qy)) return;
        const size_t noteIndex = FindNoteIndex(level, quadrant, qx, qy);
        if (noteIndex == static_cast<size_t>(-1)) return;
        color = g_notes[noteIndex].color;
    }

    static COLORREF customColors[16] = {};
    CHOOSECOLORW choose = {};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = hwnd;
    choose.rgbResult = color;
    choose.lpCustColors = customColors;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&choose)) return;

    bool changed = false;
    {
        ScopedStateLock lock;
        const size_t noteIndex = FindNoteIndex(level, quadrant, qx, qy);
        if (noteIndex == static_cast<size_t>(-1)) return;
        const COLORREF selected = choose.rgbResult & 0x00FFFFFFu;
        if (g_notes[noteIndex].color != selected) {
            g_notes[noteIndex].color = selected;
            g_notesDirty = true;
            changed = true;
        }
    }

    if (!changed) return;
    CancelNoteTooltip(hwnd);
    if (!SaveNotes()) {
        MessageBoxW(hwnd,
            L"The map note could not be saved. It will be retried when the automap closes.",
            APP_TITLE, MB_OK | MB_ICONWARNING);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void RestartAutomaticAttachment(const char*)
{
    {
        ScopedStateLock lock;
        g_attached = false;
        g_dataSeg = 0;
        g_posValid = false;
        g_inGame = false;
        g_currentDark = false;
        g_invalidCount = 0;
        g_cacheMissCount = 0;
        g_autoReconnectRequested = false;
        g_mapScrollX = 0;
        g_mapScrollY = 0;
        g_linkViewActive = false;
        g_viewRecenterPending = true;
        g_learning = false;
        g_wrootCandidates.clear();
        g_lastAnchorProbeTick = 0;
        g_lastAnchorFullScanTick = 0;
        g_status = L"Reconnecting to target process RAM automatically";
    }
    AttachOrScan();
    g_lastAnchorProbeTick = GetTickCount();
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, MAIN_TIMER_ID, 100, nullptr);
        CreateNoteTooltip(hwnd);
        return 0;

    case WM_TIMER: {
        if (wp != MAIN_TIMER_ID) return 0;

        const DWORD now = GetTickCount();
        bool needAttachProbe = false;
        bool needSave = false;
        {
            ScopedStateLock lock;
            needAttachProbe = g_cfg.enable && !g_attached &&
                              now - g_lastAnchorProbeTick >=
                                  RAM_ANCHOR_PROBE_INTERVAL_MS;
            needSave = g_cfg.autoSave && g_dirty &&
                       now - g_dirtySinceTick >= VISITED_AUTOSAVE_INTERVAL_MS;
            if (needAttachProbe) g_lastAnchorProbeTick = now;
        }
        if (needAttachProbe) AttachOrScan();
        if (needSave) SaveVisited();
        return 0;
    }

    case WM_APP_SNAPSHOT: {
        InterlockedExchange(&g_snapshotMessagePending, 0);
        bool reconnect = false;
        bool recentered = false;
        {
            ScopedStateLock lock;
            reconnect = g_autoReconnectRequested;
            g_autoReconnectRequested = false;
            recentered = g_viewRecenterPending;
            g_viewRecenterPending = false;
        }
        if (recentered) {
            StopMapDrag(hwnd);
            CancelNoteTooltip(hwnd);
        }
        if (reconnect) RestartAutomaticAttachment("snapshot validation failure");
        else InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_DPICHANGED: {
        const UINT newDpi = HIWORD(wp);
        RECT currentWindow = {};
        RECT currentClient = {};
        GetWindowRect(hwnd, &currentWindow);
        GetClientRect(hwnd, &currentClient);

        if (g_cfg.windowWidth == -1 && g_cfg.windowHeight == -1) {
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            if (suggested) {
                SetWindowPos(hwnd, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
        } else {
            const int clientWidth = (g_cfg.windowWidth == -1)
                ? std::max(1, static_cast<int>(currentClient.right - currentClient.left))
                : g_cfg.windowWidth;
            const int clientHeight = (g_cfg.windowHeight == -1)
                ? std::max(1, static_cast<int>(currentClient.bottom - currentClient.top))
                : g_cfg.windowHeight;
            RECT wanted = { 0, 0, clientWidth, clientHeight };
            if (AdjustWindowRectForDpiCompat(&wanted, WS_OVERLAPPEDWINDOW, FALSE, 0, newDpi)) {
                SetWindowPos(hwnd, nullptr, currentWindow.left, currentWindow.top,
                             wanted.right - wanted.left, wanted.bottom - wanted.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        if (g_noteTooltip) RecreateNoteTooltipFont(hwnd);
        CancelNoteTooltip(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        StopMapDrag(hwnd);
        if (!IsAltDown() && !IsCtrlDown()) {
            EditNoteAtPoint(hwnd, MouseCoordX(lp), MouseCoordY(lp));
        }
        return 0;

    case WM_LBUTTONDOWN: {
        CancelNoteTooltip(hwnd);
        const bool alt = IsAltDown();
        const bool ctrl = IsCtrlDown();
        if (alt && !ctrl) {
            CopyCoordinateAtPoint(hwnd, MouseCoordX(lp), MouseCoordY(lp));
            return 0;
        }
        if (!alt && ctrl) {
            FollowCoordinateLinkAtPoint(hwnd, MouseCoordX(lp), MouseCoordY(lp));
            return 0;
        }
        if (alt || ctrl) return 0;

        bool canDrag = false;
        {
            ScopedStateLock lock;
            canDrag = g_cfg.enable && g_inGame && g_posValid &&
                      g_pos.level < W6_LEVEL_COUNT &&
                      g_cache_valid[g_pos.level] &&
                      !(g_cfg.hideInDarkZones && g_currentDark);
        }
        if (canDrag) {
            g_draggingMap = true;
            g_dragLast.x = MouseCoordX(lp);
            g_dragLast.y = MouseCoordY(lp);
            SetCapture(hwnd);
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
        CancelNoteTooltip(hwnd);
        return 0;

    case WM_RBUTTONUP:
        ChangeNoteColorAtPoint(hwnd, MouseCoordX(lp), MouseCoordY(lp));
        return 0;

    case WM_MOUSEMOVE:
        if (!g_trackingMouseLeave) {
            TRACKMOUSEEVENT tracking = {};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = hwnd;
            if (TrackMouseEvent(&tracking)) g_trackingMouseLeave = true;
        }

        if (g_draggingMap) {
            CancelNoteTooltip(hwnd);
            if ((wp & MK_LBUTTON) == 0) {
                StopMapDrag(hwnd);
            } else {
                const int x = MouseCoordX(lp);
                const int y = MouseCoordY(lp);
                const int dx = x - g_dragLast.x;
                const int dy = y - g_dragLast.y;
                g_dragLast.x = x;
                g_dragLast.y = y;
                if (dx != 0 || dy != 0) {
                    {
                        ScopedStateLock lock;
                        g_mapScrollX = std::max(-MAP_SCROLL_LIMIT,
                                               std::min(MAP_SCROLL_LIMIT, g_mapScrollX + dx));
                        g_mapScrollY = std::max(-MAP_SCROLL_LIMIT,
                                               std::min(MAP_SCROLL_LIMIT, g_mapScrollY + dy));
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            }
        } else {
            BeginNoteHover(hwnd, MouseCoordX(lp), MouseCoordY(lp));
        }
        return 0;

    case WM_MOUSELEAVE:
        g_trackingMouseLeave = false;
        CancelNoteTooltip(hwnd);
        return 0;

    case WM_LBUTTONUP:
        StopMapDrag(hwnd);
        CancelNoteTooltip(hwnd);
        return 0;

    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        g_draggingMap = false;
        g_trackingMouseLeave = false;
        CancelNoteTooltip(hwnd);
        return 0;


    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_DESTROY:
        StopMapDrag(hwnd);
        CancelNoteTooltip(hwnd);
        KillTimer(hwnd, MAIN_TIMER_ID);
        if (g_noteTooltip && IsWindow(g_noteTooltip)) DestroyWindow(g_noteTooltip);
        g_noteTooltip = nullptr;
        if (g_noteTooltipFont) {
            DeleteObject(g_noteTooltipFont);
            g_noteTooltipFont = nullptr;
        }
        StopPollingThread();
        SaveVisited();
        SaveNotes();
        Detach();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    EnableBestDpiAwareness();
    InitializeCriticalSection(&g_stateLock);
    g_stateLockReady = true;

    SYSTEM_INFO systemInfo = {};
    GetNativeSystemInfo(&systemInfo);
    g_cfg.maxScanAddress =
        reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);

    LoadConfig();
    LoadVisited();
    LoadNotes();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = APP_CLASS;
    RegisterClassExW(&wc);

    const DWORD windowStyle = WS_OVERLAPPEDWINDOW;
    const DWORD windowExStyle = 0;
    // Create with Windows defaults first.  This lets every individual -1 setting
    // inherit the corresponding Windows-selected position or client dimension.
    g_hwnd = CreateWindowExW(windowExStyle, APP_CLASS, APP_TITLE,
                             windowStyle,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) {
        g_stateLockReady = false;
        DeleteCriticalSection(&g_stateLock);
        return 1;
    }
    ApplyInitialWindowPlacement(g_hwnd, windowStyle, windowExStyle);
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    if (!StartPollingThread()) {
        ScopedStateLock lock;
        g_status = L"Failed to start the asynchronous polling thread";
    }

    // Do the first attach immediately; the worker begins one-read snapshots
    // as soon as a live dataseg has been locked.
    if (g_cfg.enable) AttachOrScan();
    const DWORD startupTick = GetTickCount();
    g_lastAnchorProbeTick = startupTick;
    InvalidateRect(g_hwnd, nullptr, TRUE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopPollingThread();
    g_stateLockReady = false;
    DeleteCriticalSection(&g_stateLock);
    return 0;
}
