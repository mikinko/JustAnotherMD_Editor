// JAMD Editor (Just Another MD Editor) - lightweight Markdown viewer/editor.
// Native shell: top-level window + WebView2 lifecycle + file I/O + dialogs.
// All UI chrome lives in the embedded frontend (index.html), adapted from
// the JAMD_lister Total Commander plugin (see its ARCHITECTURE.md).

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <pathcch.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <strsafe.h>
#include <wincrypt.h>
#include <wrl.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"
#include "resource.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kOrigin[] = L"https://mdedit.invalid/";
constexpr wchar_t kWindowClassName[] = L"MDeditMainWindow";
constexpr wchar_t kIniSection[] = L"MDedit";

enum class DocumentKind {
    Markdown,
    Html,
    Yaml,
    Toml,
    Json,
    Text,
};

struct AppState {
    HWND hwnd = nullptr;
    std::wstring path;          // empty = untitled
    std::wstring baseDir;
    std::string document;       // bytes served as /document.md (UTF-8)
    DocumentKind kind = DocumentKind::Markdown;
    bool dirty = false;
    bool printing = false;
    bool docHadBom = false;
    bool docCrlf = true;        // EOL style to write on save
    bool closeQueryPending = false;
    bool forceClose = false;
    FILETIME docMtime = {};     // last-write time when loaded/saved (watcher)
    bool recovered = false;     // shadow copy restored; page must mark dirty
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
};

HINSTANCE g_instance = nullptr;
AppState g_app;
ComPtr<ICoreWebView2Environment> g_environment;
HWND g_placeholder = nullptr;
std::string g_indexHtml;
std::map<std::wstring, std::string> g_libCache;
std::vector<std::wstring> g_themeNames;
FILETIME g_themesStamp = {};
bool g_themesScanned = false;
std::wstring g_iniPath;

struct Settings {
    std::wstring theme = L"system";
    std::wstring codeTheme = L"auto";
    bool toc = false;
    bool split = false;
    int zoom = 100;
    int width = 980;
    int winW = 1200;
    int winH = 820;
    std::wstring cacheMode = L"system";
    std::wstring cachePath;
};

Settings g_settings;
bool g_settingsLoaded = false;

// --- small helpers (ported from JAMD_lister plugin.cpp) --------------------

std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

std::wstring ExtensionOf(const wchar_t* path)
{
    if (!path) {
        return {};
    }
    std::wstring text(path);
    const auto slash = text.find_last_of(L"\\/");
    const auto dot = text.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return {};
    }
    return ToLower(text.substr(dot + 1));
}

bool IsMarkdownExtension(const wchar_t* path)
{
    const auto ext = ExtensionOf(path);
    return ext == L"md" || ext == L"markdown" || ext == L"mdown" ||
           ext == L"mkd" || ext == L"mkdn";
}

DocumentKind KindForExtension(const wchar_t* path)
{
    if (IsMarkdownExtension(path)) {
        return DocumentKind::Markdown;
    }
    const auto ext = ExtensionOf(path);
    if (ext == L"html" || ext == L"htm") return DocumentKind::Html;
    if (ext == L"yaml" || ext == L"yml") return DocumentKind::Yaml;
    if (ext == L"toml") return DocumentKind::Toml;
    if (ext == L"json") return DocumentKind::Json;
    return DocumentKind::Text;
}

const char* DocTypeA(DocumentKind kind)
{
    switch (kind) {
    case DocumentKind::Markdown: return "markdown";
    case DocumentKind::Html: return "html";
    case DocumentKind::Yaml: return "yaml";
    case DocumentKind::Toml: return "toml";
    case DocumentKind::Json: return "json";
    default: return "text";
    }
}

bool ReadWholeFile(const wchar_t* path, std::string& out)
{
    out.clear();
    HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (64LL << 20)) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out.empty() || ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) {
        out.clear();
        return false;
    }
    out.resize(read);
    return true;
}

std::string Utf8FromWide(const std::wstring& input)
{
    if (input.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
        nullptr, 0, nullptr, nullptr);
    std::string out(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
        out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& input)
{
    if (input.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0,
        input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
        input.data(), static_cast<int>(input.size()), out.data(), needed);
    return out;
}

std::string JsonEscape(const std::string& input)
{
    std::string out;
    out.reserve(input.size() + 16);
    for (unsigned char ch : input) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buf[7] = {};
                sprintf_s(buf, "\\u%04x", ch);
                out += buf;
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
    }
    return out;
}

std::wstring JsonEscapeWide(const std::wstring& input)
{
    std::wstring out;
    out.reserve(input.size() + 8);
    for (wchar_t ch : input) {
        switch (ch) {
        case L'\\': out += L"\\\\"; break;
        case L'"': out += L"\\\""; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (ch < 0x20) {
                wchar_t buf[8] = {};
                swprintf_s(buf, L"\\u%04x", ch);
                out += buf;
            } else {
                out.push_back(ch);
            }
        }
    }
    return out;
}

int HexValue(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}

std::wstring UrlDecodeComponent(const std::wstring& encoded, bool slashToBackslash)
{
    std::string bytes;
    bytes.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == L'%' && i + 2 < encoded.size()) {
            const int hi = HexValue(encoded[i + 1]);
            const int lo = HexValue(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (slashToBackslash && encoded[i] == L'/') {
            bytes.push_back('\\');
        } else {
            wchar_t unit[2] = { encoded[i], 0 };
            int units = 1;
            if (encoded[i] >= 0xD800 && encoded[i] <= 0xDBFF && i + 1 < encoded.size()) {
                unit[1] = encoded[i + 1];
                units = 2;
            }
            char utf8[8] = {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, unit, units, utf8,
                static_cast<int>(sizeof(utf8)), nullptr, nullptr);
            bytes.append(utf8, n);
            if (units == 2) {
                ++i;
            }
        }
    }
    return Utf8ToWide(bytes);
}

std::wstring UrlDecodePath(const std::wstring& encoded)
{
    return UrlDecodeComponent(encoded, true);
}

std::wstring ModuleDirectory()
{
    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(g_instance, module, MAX_PATH);
    std::wstring dir(module);
    const auto slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}

std::wstring DirectoryOfFile(const std::wstring& path)
{
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring FileNameOf(const std::wstring& path)
{
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool DirectoryWritable(const std::wstring& dir)
{
    const std::wstring probe = dir + L"\\.mdedit_write_probe";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(file);
    return true;
}

bool StartsWithPath(const std::wstring& path, const std::wstring& root)
{
    if (path.size() < root.size()) {
        return false;
    }
    if (_wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0) {
        return false;
    }
    return path.size() == root.size() || path[root.size()] == L'\\' || path[root.size()] == L'/';
}

// Resolves an URL-encoded relative asset against rootDir, rejecting anything
// that escapes it. rootDir is passed in (not read from globals) so it is safe
// to call from a worker thread. PathCchCanonicalizeEx handles long paths.
bool ResolveUnderDir(const std::wstring& rootDir, const std::wstring& encodedAsset, std::wstring& resolved)
{
    if (rootDir.empty()) {
        return false;
    }
    const size_t suffix = encodedAsset.find_first_of(L"?#");
    const std::wstring assetOnly = suffix == std::wstring::npos ? encodedAsset : encodedAsset.substr(0, suffix);
    const std::wstring relative = UrlDecodePath(assetOnly);
    if (relative.empty() || PathIsRelativeW(relative.c_str()) == FALSE) {
        return false;
    }
    wchar_t root[MAX_PATH * 4] = {};
    wchar_t target[MAX_PATH * 4] = {};
    if (FAILED(PathCchCanonicalizeEx(root, ARRAYSIZE(root), rootDir.c_str(), PATHCCH_ALLOW_LONG_PATHS))) {
        return false;
    }
    const std::wstring combined = std::wstring(root) + L"\\" + relative;
    if (FAILED(PathCchCanonicalizeEx(target, ARRAYSIZE(target), combined.c_str(), PATHCCH_ALLOW_LONG_PATHS))) {
        return false;
    }
    if (!StartsWithPath(target, root)) {
        return false;
    }
    resolved = target;
    return true;
}

bool ResolveAssetPath(const std::wstring& encodedAsset, std::wstring& resolved)
{
    return ResolveUnderDir(g_app.baseDir, encodedAsset, resolved);
}

bool ResolveUnderRoot(const wchar_t* subdir, const std::wstring& encodedAsset, std::wstring& resolved)
{
    return ResolveUnderDir(ModuleDirectory() + L"\\" + subdir, encodedAsset, resolved);
}

const std::string* LoadLibCached(const std::wstring& path)
{
    std::wstring key = ToLower(path);
    const auto it = g_libCache.find(key);
    if (it != g_libCache.end()) {
        return &it->second;
    }
    std::string bytes;
    if (!ReadWholeFile(path.c_str(), bytes)) {
        return nullptr;
    }
    return &g_libCache.emplace(std::move(key), std::move(bytes)).first->second;
}

bool IsSafeSettingValue(const std::wstring& value)
{
    if (value.empty() || value.size() > 32) {
        return false;
    }
    for (wchar_t ch : value) {
        const bool ok = (ch >= L'a' && ch <= L'z') ||
                        (ch >= L'0' && ch <= L'9') || ch == L'-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool IsSafePathValue(const std::wstring& value)
{
    if (value.empty() || value.size() > 1024) {
        return false;
    }
    for (wchar_t ch : value) {
        if (ch < 0x20) {
            return false;
        }
    }
    return true;
}

std::vector<std::wstring> EnumerateThemes()
{
    const std::wstring dir = ModuleDirectory() + L"\\themes";
    WIN32_FILE_ATTRIBUTE_DATA attr = {};
    FILETIME stamp = {};
    if (GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &attr)) {
        stamp = attr.ftLastWriteTime;
    }
    if (g_themesScanned && CompareFileTime(&stamp, &g_themesStamp) == 0) {
        return g_themeNames;
    }
    std::vector<std::wstring> names;
    const std::wstring pattern = dir + L"\\*.json";
    WIN32_FIND_DATAW fd = {};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            std::wstring name = fd.cFileName;
            const size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos) {
                name = name.substr(0, dot);
            }
            if (IsSafeSettingValue(name)) {
                names.push_back(name);
            }
        } while (FindNextFileW(handle, &fd));
        FindClose(handle);
    }
    g_themeNames = names;
    g_themesStamp = stamp;
    g_themesScanned = true;
    return names;
}

std::wstring MimeForPath(const std::wstring& path)
{
    const std::wstring ext = ExtensionOf(path.c_str());
    if (ext == L"png") return L"image/png";
    if (ext == L"jpg" || ext == L"jpeg") return L"image/jpeg";
    if (ext == L"gif") return L"image/gif";
    if (ext == L"webp") return L"image/webp";
    if (ext == L"svg") return L"image/svg+xml";
    if (ext == L"bmp") return L"image/bmp";
    if (ext == L"html" || ext == L"htm") return L"text/html; charset=utf-8";
    if (ext == L"js" || ext == L"mjs") return L"application/javascript; charset=utf-8";
    if (ext == L"css") return L"text/css; charset=utf-8";
    if (ext == L"json") return L"application/json; charset=utf-8";
    if (ext == L"wasm") return L"application/wasm";
    if (ext == L"woff2") return L"font/woff2";
    if (ext == L"woff") return L"font/woff";
    if (ext == L"ttf") return L"font/ttf";
    return L"application/octet-stream";
}

// --- settings ---------------------------------------------------------------

std::wstring ResolveIniPath()
{
    if (!g_iniPath.empty()) {
        return g_iniPath;
    }
    const std::wstring portable = ModuleDirectory() + L"\\jamdedit.ini";
    if (DirectoryWritable(ModuleDirectory())) {
        g_iniPath = portable;
        return g_iniPath;
    }
    wchar_t* base = nullptr;
    size_t count = 0;
    _wdupenv_s(&base, &count, L"APPDATA");
    if (base) {
        std::wstring dir = std::wstring(base) + L"\\JAMDedit";
        free(base);
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        g_iniPath = dir + L"\\jamdedit.ini";
    } else {
        g_iniPath = portable;
    }
    return g_iniPath;
}

void LoadSettings()
{
    if (g_settingsLoaded) {
        return;
    }
    g_settingsLoaded = true;
    const std::wstring ini = ResolveIniPath();
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(kIniSection, L"Theme", L"system", buf, 64, ini.c_str());
    g_settings.theme = buf;
    GetPrivateProfileStringW(kIniSection, L"CodeTheme", L"auto", buf, 64, ini.c_str());
    g_settings.codeTheme = buf;
    g_settings.toc = GetPrivateProfileIntW(kIniSection, L"TOC", 0, ini.c_str()) != 0;
    g_settings.split = GetPrivateProfileIntW(kIniSection, L"Split", 0, ini.c_str()) != 0;
    g_settings.zoom = GetPrivateProfileIntW(kIniSection, L"Zoom", 100, ini.c_str());
    g_settings.width = GetPrivateProfileIntW(kIniSection, L"Width", 980, ini.c_str());
    g_settings.winW = GetPrivateProfileIntW(kIniSection, L"WindowW", 1200, ini.c_str());
    g_settings.winH = GetPrivateProfileIntW(kIniSection, L"WindowH", 820, ini.c_str());
    GetPrivateProfileStringW(kIniSection, L"CacheMode", L"system", buf, 64, ini.c_str());
    g_settings.cacheMode = buf;
    wchar_t pathBuf[1024] = {};
    GetPrivateProfileStringW(kIniSection, L"CachePath", L"", pathBuf, 1024, ini.c_str());
    g_settings.cachePath = pathBuf;
}

void WriteIni(const wchar_t* name, const std::wstring& value)
{
    const std::wstring ini = ResolveIniPath();
    if (!ini.empty()) {
        WritePrivateProfileStringW(kIniSection, name, value.c_str(), ini.c_str());
    }
}

void SaveSetting(const std::wstring& key, const std::wstring& value)
{
    LoadSettings();
    const wchar_t* name = nullptr;
    std::wstring stored = value;

    if (key == L"cachemode") {
        if (value != L"system" && value != L"custom") {
            return;
        }
        g_settings.cacheMode = value;
        name = L"CacheMode";
    } else if (key == L"cachepath") {
        if (!IsSafePathValue(value)) {
            return;
        }
        g_settings.cachePath = value;
        name = L"CachePath";
    } else if (IsSafeSettingValue(value)) {
        if (key == L"theme") {
            g_settings.theme = value;
            name = L"Theme";
        } else if (key == L"codetheme") {
            g_settings.codeTheme = value;
            name = L"CodeTheme";
        } else if (key == L"toc") {
            g_settings.toc = value == L"1";
            name = L"TOC";
            stored = g_settings.toc ? L"1" : L"0";
        } else if (key == L"split") {
            g_settings.split = value == L"1";
            name = L"Split";
            stored = g_settings.split ? L"1" : L"0";
        } else if (key == L"zoom") {
            int n = _wtoi(value.c_str());
            n = n < 50 ? 50 : (n > 200 ? 200 : n);
            g_settings.zoom = n;
            name = L"Zoom";
            stored = std::to_wstring(n);
        } else if (key == L"width") {
            int n = _wtoi(value.c_str());
            n = n < 600 ? 600 : (n > 2000 ? 2000 : n);
            g_settings.width = n;
            name = L"Width";
            stored = std::to_wstring(n);
        } else {
            return;
        }
    } else {
        return;
    }

    if (name) {
        WriteIni(name, stored);
    }
}

// --- recent files (INI section [Recent], File0..File9, newest first) -------

constexpr wchar_t kRecentSection[] = L"Recent";
constexpr int kRecentMax = 10;

std::vector<std::wstring> LoadRecent()
{
    const std::wstring ini = ResolveIniPath();
    std::vector<std::wstring> items;
    wchar_t buf[1024];
    for (int i = 0; i < kRecentMax; ++i) {
        const std::wstring key = L"File" + std::to_wstring(i);
        buf[0] = 0;
        GetPrivateProfileStringW(kRecentSection, key.c_str(), L"", buf, 1024, ini.c_str());
        if (buf[0]) {
            items.emplace_back(buf);
        }
    }
    return items;
}

void AddRecent(const std::wstring& path)
{
    if (path.empty()) {
        return;
    }
    const std::wstring ini = ResolveIniPath();
    if (ini.empty()) {
        return;
    }
    std::vector<std::wstring> items = LoadRecent();
    items.erase(std::remove_if(items.begin(), items.end(),
        [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; }),
        items.end());
    items.insert(items.begin(), path);
    if (items.size() > kRecentMax) {
        items.resize(kRecentMax);
    }
    for (int i = 0; i < kRecentMax; ++i) {
        const std::wstring key = L"File" + std::to_wstring(i);
        WritePrivateProfileStringW(kRecentSection, key.c_str(),
            i < static_cast<int>(items.size()) ? items[i].c_str() : nullptr, ini.c_str());
    }
}

// --- OS dark mode / titlebar -------------------------------------------------

bool SystemPrefersDark()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value == 0;
    }
    return false;
}

void ApplyTitlebarTheme(HWND hwnd)
{
    const BOOL dark = SystemPrefersDark() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}

// --- document state ----------------------------------------------------------

void UpdateTitle()
{
    std::wstring name = g_app.path.empty() ? L"Untitled" : FileNameOf(g_app.path);
    std::wstring title = name + (g_app.dirty ? L"*" : L"") + L" — JAMD Editor";
    SetWindowTextW(g_app.hwnd, title.c_str());
}

void DetectDocFormat(const std::string& bytes)
{
    g_app.docHadBom = bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF;
    const size_t lf = bytes.find('\n');
    g_app.docCrlf = lf == std::string::npos || (lf > 0 && bytes[lf - 1] == '\r');
}

// --- autosave shadow copies (crash recovery) --------------------------------

bool GetFileMtime(const std::wstring& path, FILETIME& ft)
{
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    ft = fad.ftLastWriteTime;
    return true;
}

std::wstring ShadowDir()
{
    wchar_t* base = nullptr;
    size_t count = 0;
    _wdupenv_s(&base, &count, L"APPDATA");
    std::wstring dir = base ? std::wstring(base) + L"\\JAMDedit\\recover"
                            : ModuleDirectory() + L"\\recover";
    if (base) {
        free(base);
    }
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

std::wstring ShadowPathFor(const std::wstring& path)
{
    std::wstring name = path.empty() ? L"_untitled" : path;
    for (wchar_t& ch : name) {
        if (ch == L'\\' || ch == L'/' || ch == L':') {
            ch = L'_';
        }
    }
    return ShadowDir() + L"\\" + name + L".autosave";
}

void DeleteShadow(const std::wstring& docPath)
{
    DeleteFileW(ShadowPathFor(docPath).c_str());
}

// A newer shadow copy than the file itself means a previous session died
// with unsaved changes: offer to restore them.
void CheckShadowRecovery()
{
    const std::wstring shadow = ShadowPathFor(g_app.path);
    FILETIME shadowTime;
    if (!GetFileMtime(shadow, shadowTime)) {
        return;
    }
    FILETIME docTime = {};
    bool newer = true;
    if (!g_app.path.empty() && GetFileMtime(g_app.path, docTime)) {
        newer = CompareFileTime(&shadowTime, &docTime) > 0;
    }
    std::string bytes;
    if (!newer || !ReadWholeFile(shadow.c_str(), bytes)) {
        DeleteFileW(shadow.c_str());
        return;
    }
    TASKDIALOGCONFIG config = { sizeof(config) };
    config.hwndParent = g_app.hwnd;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = L"JAMD Editor";
    config.pszMainInstruction = L"Unsaved changes from a previous session were found.";
    config.pszContent = L"Restore the recovered version, or discard it and load the file from disk?";
    const TASKDIALOG_BUTTON buttons[] = {
        { IDYES, L"Restore recovered version" },
        { IDNO, L"Discard it" },
    };
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);
    config.nDefaultButton = IDYES;
    int pressed = IDNO;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) {
        pressed = IDNO;
    }
    if (pressed == IDYES) {
        if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF) {
            bytes.erase(0, 3);
        }
        g_app.document = std::move(bytes);
        g_app.dirty = true;
        g_app.recovered = true;
        UpdateTitle();
    } else {
        DeleteFileW(shadow.c_str());
    }
}

void SetDocument(const std::wstring& path, std::string bytes)
{
    DetectDocFormat(bytes);
    if (g_app.docHadBom) {
        bytes.erase(0, 3);  // frontend never sees the BOM; re-added on save
    }
    g_app.path = path;
    g_app.baseDir = path.empty() ? std::wstring() : DirectoryOfFile(path);
    g_app.document = std::move(bytes);
    g_app.kind = path.empty() ? DocumentKind::Markdown : KindForExtension(path.c_str());
    g_app.dirty = false;
    GetFileMtime(path, g_app.docMtime);
    UpdateTitle();
    AddRecent(path);
    CheckShadowRecovery();
}

std::string NormalizeForSave(const std::wstring& text)
{
    // Frontend textarea yields \n; restore the document's original EOL style.
    std::string utf8 = Utf8FromWide(text);
    std::string normalized;
    normalized.reserve(utf8.size() + (g_app.docCrlf ? utf8.size() / 16 : 0));
    for (size_t i = 0; i < utf8.size(); ++i) {
        if (utf8[i] == '\r') {
            continue;  // strip any stray CRs first
        }
        if (utf8[i] == '\n' && g_app.docCrlf) {
            normalized += "\r\n";
        } else {
            normalized.push_back(utf8[i]);
        }
    }
    if (g_app.docHadBom) {
        normalized.insert(0, "\xEF\xBB\xBF");
    }
    return normalized;
}

bool WriteWholeFile(const std::wstring& path, const std::string& bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = bytes.empty() ||
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == bytes.size();
}

// --- config for the frontend --------------------------------------------------

std::string ConfigJson()
{
    LoadSettings();
    std::string json = "{";
    json += "\"app\":\"JAMD Editor\",";
    json += "\"editor\":true,";
    json += "\"docType\":\"";
    json += DocTypeA(g_app.kind);
    json += "\",";
    json += "\"dark\":";
    json += SystemPrefersDark() ? "true" : "false";
    json += ",";
    json += "\"theme\":\"" + JsonEscape(Utf8FromWide(g_settings.theme)) + "\",";
    json += "\"codeTheme\":\"" + JsonEscape(Utf8FromWide(g_settings.codeTheme)) + "\",";
    json += "\"toc\":";
    json += g_settings.toc ? "true" : "false";
    json += ",";
    json += "\"split\":";
    json += g_settings.split ? "true" : "false";
    json += ",";
    json += "\"zoom\":" + std::to_string(g_settings.zoom) + ",";
    json += "\"width\":" + std::to_string(g_settings.width) + ",";
    json += "\"cacheMode\":\"" + JsonEscape(Utf8FromWide(g_settings.cacheMode)) + "\",";
    json += "\"cachePath\":\"" + JsonEscape(Utf8FromWide(g_settings.cachePath)) + "\",";
    if (g_app.recovered) {
        json += "\"recovered\":true,";
        g_app.recovered = false;  // one-shot: only the first load after restore
    }
    json += "\"siblings\":[";
    if (!g_app.baseDir.empty()) {
        WIN32_FIND_DATAW fd = {};
        HANDLE find = FindFirstFileW((g_app.baseDir + L"\\*").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            bool first = true;
            int count = 0;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    continue;
                }
                if (!IsMarkdownExtension(fd.cFileName)) {
                    continue;
                }
                if (++count > 200) {
                    break;
                }
                if (!first) {
                    json += ",";
                }
                first = false;
                json += "\"" + JsonEscape(Utf8FromWide(g_app.baseDir + L"\\" + fd.cFileName)) + "\"";
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }
    json += "],";
    json += "\"recent\":[";
    {
        const std::vector<std::wstring> recent = LoadRecent();
        bool first = true;
        for (const std::wstring& item : recent) {
            // Deliberately no existence check: one dead network path would
            // block the UI thread here. OpenPath reports missing files.
            if (!first) {
                json += ",";
            }
            first = false;
            json += "\"" + JsonEscape(Utf8FromWide(item)) + "\"";
        }
    }
    json += "],";
    json += "\"themes\":[";
    const std::vector<std::wstring> themeNames = EnumerateThemes();
    for (size_t i = 0; i < themeNames.size(); ++i) {
        if (i) {
            json += ",";
        }
        json += "\"" + JsonEscape(Utf8FromWide(themeNames[i])) + "\"";
    }
    json += "],";
    json += "\"path\":\"" + JsonEscape(Utf8FromWide(g_app.path)) + "\"}";
    return json;
}

// --- WebView helpers ----------------------------------------------------------

void PostJson(const std::wstring& json)
{
    if (g_app.webview) {
        g_app.webview->PostWebMessageAsJson(json.c_str());
    }
}

void ReloadDocumentInPage()
{
    PostJson(L"{\"reload\":true}");
}

void NotifySaved()
{
    std::wstring msg = L"{\"saved\":true,\"path\":\"" + JsonEscapeWide(g_app.path) +
        L"\",\"name\":\"" + JsonEscapeWide(FileNameOf(g_app.path)) + L"\"}";
    PostJson(msg);
}

// --- dialogs -------------------------------------------------------------------

const COMDLG_FILTERSPEC kFileFilters[] = {
    { L"Markdown files", L"*.md;*.markdown;*.mdown;*.mkd;*.mkdn" },
    { L"Text files", L"*.txt" },
    { L"All supported", L"*.md;*.markdown;*.mdown;*.mkd;*.mkdn;*.txt;*.html;*.htm;*.json;*.yaml;*.yml;*.toml" },
    { L"All files", L"*.*" },
};

bool OpenFileDialog(std::wstring& out)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    dialog->SetFileTypes(ARRAYSIZE(kFileFilters), kFileFilters);
    dialog->SetFileTypeIndex(3);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (FAILED(dialog->Show(g_app.hwnd))) {
        return false;
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return false;
    }
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
        return false;
    }
    out = path;
    CoTaskMemFree(path);
    return true;
}

bool SaveFileDialog(std::wstring& out)
{
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    dialog->SetFileTypes(ARRAYSIZE(kFileFilters), kFileFilters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"md");
    if (!g_app.path.empty()) {
        dialog->SetFileName(FileNameOf(g_app.path).c_str());
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    if (FAILED(dialog->Show(g_app.hwnd))) {
        return false;
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return false;
    }
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
        return false;
    }
    out = path;
    CoTaskMemFree(path);
    return true;
}

// Save dialog with a single filter, e.g. ("pdf", "PDF document", "*.pdf").
bool SaveDialogGeneric(const wchar_t* ext, const wchar_t* filterName,
    const wchar_t* filterPattern, const std::wstring& suggestedName, std::wstring& out)
{
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    const COMDLG_FILTERSPEC filter[] = { { filterName, filterPattern }, { L"All files", L"*.*" } };
    dialog->SetFileTypes(ARRAYSIZE(filter), filter);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(ext);
    if (!suggestedName.empty()) {
        dialog->SetFileName(suggestedName.c_str());
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    if (FAILED(dialog->Show(g_app.hwnd))) {
        return false;
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return false;
    }
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
        return false;
    }
    out = path;
    CoTaskMemFree(path);
    return true;
}

// Document file name without extension, for export suggestions.
std::wstring ExportBaseName()
{
    std::wstring name = g_app.path.empty() ? L"Untitled" : FileNameOf(g_app.path);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        name.erase(dot);
    }
    return name;
}

bool PickFolderDialog(std::wstring& out)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (FAILED(dialog->Show(g_app.hwnd))) {
        return false;
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return false;
    }
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
        return false;
    }
    out = path;
    CoTaskMemFree(path);
    return true;
}

// Save / Discard / Cancel. Returns IDYES (save), IDNO (discard), IDCANCEL.
int AskSaveChanges()
{
    const std::wstring name = g_app.path.empty() ? L"Untitled" : FileNameOf(g_app.path);
    const std::wstring line = L"Save changes to \"" + name + L"\"?";
    TASKDIALOGCONFIG config = { sizeof(config) };
    config.hwndParent = g_app.hwnd;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = 0;
    config.pszWindowTitle = L"JAMD Editor";
    config.pszMainInstruction = line.c_str();
    config.pszContent = L"Your changes will be lost if you don't save them.";
    const TASKDIALOG_BUTTON buttons[] = {
        { IDYES, L"Save" },
        { IDNO, L"Don't save" },
        { IDCANCEL, L"Cancel" },
    };
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);
    config.nDefaultButton = IDYES;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) {
        pressed = IDCANCEL;
    }
    return pressed;
}

// --- save / open flows ----------------------------------------------------------

// Writes `text` to g_app.path (SaveAs dialog when untitled or forced).
// Returns false when the user cancelled or the write failed.
bool SaveDocument(const std::wstring& text, bool forceDialog)
{
    const std::wstring previousPath = g_app.path;
    std::wstring target = g_app.path;
    if (forceDialog || target.empty()) {
        if (!SaveFileDialog(target)) {
            return false;
        }
    }
    const std::string bytes = NormalizeForSave(text);
    if (!WriteWholeFile(target, bytes)) {
        MessageBoxW(g_app.hwnd, (L"Could not write file:\n" + target).c_str(),
            L"JAMD Editor", MB_ICONERROR);
        return false;
    }
    const bool pathChanged = _wcsicmp(target.c_str(), g_app.path.c_str()) != 0;
    g_app.path = target;
    g_app.baseDir = DirectoryOfFile(target);
    g_app.kind = KindForExtension(target.c_str());
    g_app.document = bytes;
    DetectDocFormat(bytes);
    if (g_app.docHadBom) {
        g_app.document.erase(0, 3);
    }
    g_app.dirty = false;
    GetFileMtime(target, g_app.docMtime);
    DeleteShadow(previousPath);
    DeleteShadow(target);
    UpdateTitle();
    NotifySaved();
    AddRecent(target);
    if (pathChanged) {
        // baseDir moved: relative assets resolve differently now.
        ReloadDocumentInPage();
    }
    return true;
}

// If dirty, prompt; optionally saves. Returns true when the caller may proceed.
bool ConfirmLoseChanges(bool dirty, const std::wstring& text)
{
    if (!dirty) {
        return true;
    }
    const int choice = AskSaveChanges();
    if (choice == IDCANCEL) {
        return false;
    }
    if (choice == IDYES) {
        return SaveDocument(text, false);
    }
    return true;  // discard
}

void OpenPath(const std::wstring& path)
{
    std::string bytes;
    if (!ReadWholeFile(path.c_str(), bytes)) {
        MessageBoxW(g_app.hwnd, (L"Could not open file:\n" + path).c_str(),
            L"JAMD Editor", MB_ICONERROR);
        return;
    }
    SetDocument(path, std::move(bytes));
    ReloadDocumentInPage();
}

void OpenViaDialog()
{
    std::wstring picked;
    if (OpenFileDialog(picked)) {
        OpenPath(picked);
    }
}

// Poll the open file for external modification (other editors, git, sync).
void CheckExternalChange()
{
    if (g_app.path.empty() || g_app.printing || !g_app.webview) {
        return;
    }
    if (!IsWindowEnabled(g_app.hwnd)) {
        return;  // a modal dialog is up; don't stack another
    }
    FILETIME ft;
    if (!GetFileMtime(g_app.path, ft) || CompareFileTime(&ft, &g_app.docMtime) == 0) {
        return;
    }
    if (!g_app.dirty) {
        std::string bytes;
        if (ReadWholeFile(g_app.path.c_str(), bytes)) {
            SetDocument(g_app.path, std::move(bytes));
            ReloadDocumentInPage();
        } else {
            g_app.docMtime = ft;  // unreadable mid-write; retry next tick would loop
        }
        return;
    }
    g_app.docMtime = ft;  // one prompt per external change
    TASKDIALOGCONFIG config = { sizeof(config) };
    config.hwndParent = g_app.hwnd;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = L"JAMD Editor";
    config.pszMainInstruction = L"The file was changed on disk.";
    config.pszContent = L"Reload it and lose your unsaved edits, or keep your version?";
    const TASKDIALOG_BUTTON buttons[] = {
        { IDYES, L"Reload from disk" },
        { IDNO, L"Keep my version" },
    };
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);
    config.nDefaultButton = IDNO;
    int pressed = IDNO;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) {
        pressed = IDNO;
    }
    if (pressed == IDYES) {
        std::string bytes;
        if (ReadWholeFile(g_app.path.c_str(), bytes)) {
            SetDocument(g_app.path, std::move(bytes));
            ReloadDocumentInPage();
        }
    }
}

bool ResolveMarkdownTarget(const std::wstring& href, std::wstring& resolved)
{
    if (g_app.baseDir.empty()) {
        return false;
    }
    if (href.empty() || href.front() == L'#' || href.rfind(L"//", 0) == 0) {
        return false;
    }
    const size_t colon = href.find(L':');
    const bool looksLikeDrivePath = colon == 1 && href.size() >= 3 &&
        ((href[2] == L'\\') || (href[2] == L'/'));
    if (colon != std::wstring::npos && !looksLikeDrivePath) {
        return false;
    }
    const size_t suffix = href.find_first_of(L"?#");
    const std::wstring pathOnly = suffix == std::wstring::npos ? href : href.substr(0, suffix);
    const std::wstring decoded = UrlDecodePath(pathOnly);
    if (decoded.empty() || !IsMarkdownExtension(decoded.c_str())) {
        return false;
    }
    wchar_t combined[MAX_PATH * 4] = {};
    const std::wstring candidate = PathIsRelativeW(decoded.c_str())
        ? (g_app.baseDir + L"\\" + decoded) : decoded;
    if (FAILED(PathCchCanonicalizeEx(combined, ARRAYSIZE(combined), candidate.c_str(), PATHCCH_ALLOW_LONG_PATHS))) {
        return false;
    }
    resolved = combined;
    return true;
}

void FinishClose()
{
    // Deliberate close (saved or discarded): the shadow copy is obsolete.
    DeleteShadow(g_app.path);
    g_app.forceClose = true;
    DestroyWindow(g_app.hwnd);
}

// Page replied to a close query (Esc or WM_CLOSE): "close|<0/1>|<text>"
void HandleCloseReply(bool dirty, const std::wstring& text)
{
    g_app.closeQueryPending = false;
    if (ConfirmLoseChanges(dirty, text)) {
        FinishClose();
    }
}

// --- WebView2 message bridge -----------------------------------------------------

// Splits "cmd|arg1|rest" -> returns cmd, fills args (rest keeps embedded '|').
std::wstring SplitMessage(const std::wstring& message, int fixedParts, std::vector<std::wstring>& args)
{
    args.clear();
    size_t start = message.find(L'|');
    const std::wstring cmd = message.substr(0, start);
    if (start == std::wstring::npos) {
        return cmd;
    }
    ++start;
    // fixedParts counts the command plus every field. We consume one delimiter
    // per intermediate field; the final field is taken verbatim so it may itself
    // contain '|' (e.g. a Markdown table in the document text). Consuming an
    // extra delimiter here would truncate that final field at its first pipe.
    for (int i = 2; i < fixedParts; ++i) {
        const size_t bar = message.find(L'|', start);
        if (bar == std::wstring::npos) {
            break;
        }
        args.push_back(message.substr(start, bar - start));
        start = bar + 1;
    }
    args.push_back(message.substr(start));
    return cmd;
}

// Defined below HandleWebMessage.
void PasteImage(const std::wstring& ext, const std::wstring& base64);
void InsertImageFromFile();
void ExportPdf();
void ExportHtml(const std::wstring& html);
void CheckLinks(const std::wstring& list);

void HandleWebMessage(const std::wstring& message)
{
    std::vector<std::wstring> args;

    if (message.rfind(L"save|", 0) == 0) {
        SaveDocument(message.substr(5), false);
    } else if (message.rfind(L"saveas|", 0) == 0) {
        SaveDocument(message.substr(7), true);
    } else if (message.rfind(L"open|", 0) == 0) {
        // open|<dirty>|<text>
        SplitMessage(message, 3, args);
        const bool dirty = args.size() >= 1 && args[0] == L"1";
        const std::wstring text = args.size() >= 2 ? args[1] : L"";
        if (ConfirmLoseChanges(dirty, text)) {
            OpenViaDialog();
        }
    } else if (message.rfind(L"close|", 0) == 0) {
        // close|<dirty>|<text>
        SplitMessage(message, 3, args);
        const bool dirty = args.size() >= 1 && args[0] == L"1";
        const std::wstring text = args.size() >= 2 ? args[1] : L"";
        HandleCloseReply(dirty, text);
    } else if (message.rfind(L"navigate|", 0) == 0) {
        // navigate|<encoded href>|<dirty>|<text>
        SplitMessage(message, 4, args);
        if (args.size() >= 1) {
            const std::wstring href = UrlDecodeComponent(args[0], false);
            const bool dirty = args.size() >= 2 && args[1] == L"1";
            const std::wstring text = args.size() >= 3 ? args[2] : L"";
            std::wstring target;
            if (ResolveMarkdownTarget(href, target) && ConfirmLoseChanges(dirty, text)) {
                OpenPath(target);
            }
        }
    } else if (message.rfind(L"openpath|", 0) == 0) {
        // openpath|<encoded path>|<dirty>|<text>  (recent-files menu)
        SplitMessage(message, 4, args);
        if (args.size() >= 1) {
            const std::wstring path = UrlDecodeComponent(args[0], false);
            const bool dirty = args.size() >= 2 && args[1] == L"1";
            const std::wstring text = args.size() >= 3 ? args[2] : L"";
            if (!path.empty() && ConfirmLoseChanges(dirty, text)) {
                OpenPath(path);
            }
        }
    } else if (message.rfind(L"dirty|", 0) == 0) {
        const bool wasDirty = g_app.dirty;
        g_app.dirty = message.substr(6) == L"1";
        if (wasDirty != g_app.dirty) {
            UpdateTitle();
        }
    } else if (message.rfind(L"external|", 0) == 0) {
        const std::wstring href = UrlDecodeComponent(message.substr(9), false);
        if (!href.empty()) {
            ShellExecuteW(nullptr, L"open", href.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (message.rfind(L"printing|", 0) == 0) {
        g_app.printing = message.substr(9) == L"1";
    } else if (message.rfind(L"shadow|", 0) == 0) {
        // Autosave shadow copy for crash recovery (raw UTF-8, LF, no BOM).
        WriteWholeFile(ShadowPathFor(g_app.path), Utf8FromWide(message.substr(7)));
    } else if (message.rfind(L"pasteimage|", 0) == 0) {
        // pasteimage|<ext>|<base64>
        SplitMessage(message, 3, args);
        if (args.size() >= 2) {
            PasteImage(args[0], args[1]);
        }
    } else if (message == L"insertimage") {
        InsertImageFromFile();
    } else if (message == L"exportpdf") {
        ExportPdf();
    } else if (message.rfind(L"exporthtml|", 0) == 0) {
        ExportHtml(message.substr(11));
    } else if (message.rfind(L"checklinks|", 0) == 0) {
        CheckLinks(message.substr(11));
    } else if (message.rfind(L"set|", 0) == 0) {
        const std::wstring rest = message.substr(4);
        const size_t bar = rest.find(L'|');
        if (bar != std::wstring::npos) {
            SaveSetting(rest.substr(0, bar), rest.substr(bar + 1));
        }
    } else if (message == L"pickfolder") {
        std::wstring picked;
        if (PickFolderDialog(picked) && !picked.empty()) {
            SaveSetting(L"cachemode", L"custom");
            SaveSetting(L"cachepath", picked);
            PostJson(L"{\"cacheMode\":\"custom\",\"cachePath\":\"" + JsonEscapeWide(picked) + L"\"}");
        }
    }
}

// Clipboard image pasted in the editor: decode base64, store next to the
// document under img\, tell the page the relative path to insert.
void PasteImage(const std::wstring& ext, const std::wstring& base64)
{
    if (g_app.baseDir.empty()) {
        MessageBoxW(g_app.hwnd,
            L"Save the document first so pasted images can be stored next to it.",
            L"JAMD Editor", MB_ICONINFORMATION);
        return;
    }
    std::wstring safeExt = ToLower(ext);
    if (safeExt.empty() || safeExt.size() > 4 ||
        safeExt.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789") != std::wstring::npos) {
        safeExt = L"png";
    }
    const std::string ascii = Utf8FromWide(base64);
    DWORD binLen = 0;
    if (!CryptStringToBinaryA(ascii.c_str(), static_cast<DWORD>(ascii.size()),
            CRYPT_STRING_BASE64, nullptr, &binLen, nullptr, nullptr) || !binLen) {
        return;
    }
    std::string bin(binLen, '\0');
    if (!CryptStringToBinaryA(ascii.c_str(), static_cast<DWORD>(ascii.size()),
            CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(&bin[0]), &binLen, nullptr, nullptr)) {
        return;
    }
    bin.resize(binLen);
    const std::wstring dir = g_app.baseDir + L"\\img";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t name[64] = {};
    StringCchPrintfW(name, ARRAYSIZE(name), L"pasted-%04u%02u%02u-%02u%02u%02u.%s",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, safeExt.c_str());
    if (!WriteWholeFile(dir + L"\\" + name, bin)) {
        MessageBoxW(g_app.hwnd, L"Could not write the pasted image.", L"JAMD Editor", MB_ICONERROR);
        return;
    }
    PostJson(L"{\"pasted\":\"img/" + JsonEscapeWide(name) + L"\"}");
}

// Toolbar "insert image": pick an image file, copy it beside the document
// under img\, tell the page the relative path to insert.
void InsertImageFromFile()
{
    if (g_app.baseDir.empty()) {
        MessageBoxW(g_app.hwnd,
            L"Save the document first so inserted images can be stored next to it.",
            L"JAMD Editor", MB_ICONINFORMATION);
        return;
    }
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return;
    }
    const COMDLG_FILTERSPEC filter[] = {
        { L"Images", L"*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.svg" },
        { L"All files", L"*.*" },
    };
    dialog->SetFileTypes(ARRAYSIZE(filter), filter);
    dialog->SetFileTypeIndex(1);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (FAILED(dialog->Show(g_app.hwnd))) {
        return;
    }
    ComPtr<IShellItem> item;
    PWSTR raw = nullptr;
    if (FAILED(dialog->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) {
        return;
    }
    const std::wstring source = raw;
    CoTaskMemFree(raw);

    const std::wstring dir = g_app.baseDir + L"\\img";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring baseName = FileNameOf(source);
    const size_t dot = baseName.find_last_of(L'.');
    const std::wstring stem = dot == std::wstring::npos ? baseName : baseName.substr(0, dot);
    const std::wstring ext = dot == std::wstring::npos ? std::wstring() : baseName.substr(dot);
    std::wstring name = baseName;
    for (int n = 1; PathFileExistsW((dir + L"\\" + name).c_str()); ++n) {
        wchar_t suffix[16] = {};
        StringCchPrintfW(suffix, ARRAYSIZE(suffix), L"-%d", n);
        name = stem + suffix + ext;
    }
    if (!CopyFileW(source.c_str(), (dir + L"\\" + name).c_str(), TRUE)) {
        MessageBoxW(g_app.hwnd, L"Could not copy the image next to the document.",
            L"JAMD Editor", MB_ICONERROR);
        return;
    }
    PostJson(L"{\"pasted\":\"img/" + JsonEscapeWide(name) + L"\"}");
}

void ExportPdf()
{
    if (!g_app.webview) {
        return;
    }
    std::wstring out;
    if (!SaveDialogGeneric(L"pdf", L"PDF document", L"*.pdf", ExportBaseName() + L".pdf", out)) {
        return;
    }
    ComPtr<ICoreWebView2_7> wv7;
    if (FAILED(g_app.webview.As(&wv7))) {
        MessageBoxW(g_app.hwnd, L"PDF export needs a newer WebView2 runtime.",
            L"JAMD Editor", MB_ICONERROR);
        return;
    }
    wv7->PrintToPdf(out.c_str(), nullptr,
        Callback<ICoreWebView2PrintToPdfCompletedHandler>(
            [](HRESULT hr, BOOL ok) -> HRESULT {
                if (FAILED(hr) || !ok) {
                    MessageBoxW(g_app.hwnd, L"PDF export failed.", L"JAMD Editor", MB_ICONERROR);
                }
                return S_OK;
            }).Get());
}

void ExportHtml(const std::wstring& html)
{
    std::wstring out;
    if (!SaveDialogGeneric(L"html", L"HTML document", L"*.html;*.htm",
            ExportBaseName() + L".html", out)) {
        return;
    }
    if (!WriteWholeFile(out, Utf8FromWide(html))) {
        MessageBoxW(g_app.hwnd, (L"Could not write file:\n" + out).c_str(),
            L"JAMD Editor", MB_ICONERROR);
    }
}

// "checklinks|<enc1>|<enc2>|..." -> {"linkcheck":[true,false,...]}
// The existence checks hit the filesystem (possibly a dead network share),
// so they run on a worker thread; the result is marshalled back to the UI
// thread via kMsgLinkCheck. A generation counter drops superseded replies.
constexpr UINT kMsgLinkCheck = WM_APP + 1;
std::atomic<unsigned> g_linkCheckGen{ 0 };

void CheckLinks(const std::wstring& list)
{
    const unsigned gen = ++g_linkCheckGen;
    std::thread([gen, list, baseDir = g_app.baseDir]() {
        std::wstring json = L"{\"linkcheck\":[";
        bool first = true;
        size_t start = 0;
        while (start <= list.size()) {
            const size_t bar = list.find(L'|', start);
            const std::wstring part = bar == std::wstring::npos
                ? list.substr(start) : list.substr(start, bar - start);
            bool exists = false;
            if (!part.empty()) {
                std::wstring resolved;
                exists = ResolveUnderDir(baseDir, part, resolved) && PathFileExistsW(resolved.c_str());
            }
            if (!first) {
                json += L",";
            }
            first = false;
            json += exists ? L"true" : L"false";
            if (bar == std::wstring::npos) {
                break;
            }
            start = bar + 1;
        }
        json += L"]}";
        if (gen != g_linkCheckGen.load()) {
            return;  // a newer lint pass started; this result is stale
        }
        auto* payload = new std::wstring(std::move(json));
        if (!PostMessageW(g_app.hwnd, kMsgLinkCheck, gen, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }).detach();
}

// Page posted "dropfiles|<dirty>|<text>" with the dropped files attached
// as AdditionalObjects (drag & drop onto the window).
void HandleDropMessage(const std::wstring& message, const std::wstring& droppedPath)
{
    std::vector<std::wstring> args;
    SplitMessage(message, 3, args);
    const bool dirty = args.size() >= 1 && args[0] == L"1";
    const std::wstring text = args.size() >= 2 ? args[1] : L"";
    if (!droppedPath.empty() && ConfirmLoseChanges(dirty, text)) {
        OpenPath(droppedPath);
    }
}

// --- WebView2 resource serving ------------------------------------------------------

std::string LoadRcData(int id)
{
    HRSRC res = FindResourceW(g_instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (res) {
        HGLOBAL loaded = LoadResource(g_instance, res);
        if (loaded) {
            const void* data = LockResource(loaded);
            const DWORD size = SizeofResource(g_instance, res);
            return std::string(static_cast<const char*>(data), size);
        }
    }
    return std::string();
}

const std::string& LoadIndexHtml()
{
    if (g_indexHtml.empty()) {
        g_indexHtml = LoadRcData(IDR_INDEX_HTML);
    }
    return g_indexHtml;
}

const std::string& LoadLogoPng()
{
    static std::string logo;
    if (logo.empty()) {
        logo = LoadRcData(IDR_LOGO_PNG);
    }
    return logo;
}

HRESULT CreateResponse(ICoreWebView2Environment* env, const std::string& body,
    const wchar_t* mime, ICoreWebView2WebResourceResponse** response,
    int status = 200, const wchar_t* reason = L"OK",
    const wchar_t* cacheControl = L"no-store",
    const std::wstring& extraHeaders = {})
{
    IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(body.data()),
        static_cast<UINT>(body.size()));
    if (!stream) {
        return E_OUTOFMEMORY;
    }
    std::wstring headers = L"Content-Type: ";
    headers += mime;
    headers += L"\r\nCache-Control: ";
    headers += cacheControl;
    if (!extraHeaders.empty()) {
        headers += L"\r\n";
        headers += extraHeaders;
    }
    const HRESULT hr = env->CreateWebResourceResponse(stream, status, reason, headers.c_str(), response);
    stream->Release();
    return hr;
}

void RegisterResourceHandler()
{
    g_app.webview->AddWebResourceRequestedFilter(
        L"https://mdedit.invalid/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

    EventRegistrationToken token = {};
    g_app.webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                ComPtr<ICoreWebView2WebResourceRequest> request;
                args->get_Request(&request);

                LPWSTR rawUri = nullptr;
                request->get_Uri(&rawUri);
                std::wstring uri = rawUri ? rawUri : L"";
                CoTaskMemFree(rawUri);

                std::string body;
                const std::string* payload = &body;
                const wchar_t* mime = L"text/plain; charset=utf-8";
                std::wstring mimeOwned;
                const wchar_t* cacheControl = L"no-store";
                std::wstring extraHeaders;
                int status = 200;
                const wchar_t* reason = L"OK";

                // Route on the exact path under the synthetic origin. Substring
                // matching would let a document asset like img/logo.png collide
                // with the built-in /logo.png (or config.json, document.md).
                const std::wstring path = uri.rfind(kOrigin, 0) == 0
                    ? uri.substr(ARRAYSIZE(kOrigin) - 1) : std::wstring();
                const size_t pathSuffix = path.find_first_of(L"?#");
                const std::wstring bare = pathSuffix == std::wstring::npos
                    ? path : path.substr(0, pathSuffix);

                if (path.rfind(L"htmlview/", 0) == 0) {
                    std::wstring filePath;
                    if (ResolveAssetPath(path.substr(9), filePath) &&
                        ReadWholeFile(filePath.c_str(), body)) {
                        mimeOwned = MimeForPath(filePath);
                        mime = mimeOwned.c_str();
                    } else {
                        body = "File not found";
                        status = 404;
                        reason = L"Not Found";
                    }
                } else if (bare == L"logo.png") {
                    payload = &LoadLogoPng();
                    mime = L"image/png";
                    cacheControl = L"max-age=86400";
                } else if (bare == L"document.md") {
                    payload = &g_app.document;
                    mime = L"text/markdown; charset=utf-8";
                } else if (bare == L"config.json") {
                    body = ConfigJson();
                    mime = L"application/json; charset=utf-8";
                } else if (path.rfind(L"asset/", 0) == 0) {
                    // ETag (mtime+size) with no-cache: the browser revalidates
                    // each use and unchanged files come back 304 without a body,
                    // so live re-renders don't re-read every image from disk.
                    std::wstring assetPath;
                    WIN32_FILE_ATTRIBUTE_DATA fad = {};
                    if (ResolveAssetPath(path.substr(6), assetPath) &&
                        GetFileAttributesExW(assetPath.c_str(), GetFileExInfoStandard, &fad) &&
                        !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        wchar_t etag[48] = {};
                        swprintf_s(etag, L"\"%08lx%08lx-%lx%lx\"",
                            fad.ftLastWriteTime.dwHighDateTime, fad.ftLastWriteTime.dwLowDateTime,
                            fad.nFileSizeHigh, fad.nFileSizeLow);
                        extraHeaders = std::wstring(L"ETag: ") + etag;
                        cacheControl = L"no-cache";
                        mimeOwned = MimeForPath(assetPath);
                        mime = mimeOwned.c_str();
                        ComPtr<ICoreWebView2HttpRequestHeaders> reqHeaders;
                        LPWSTR match = nullptr;
                        if (SUCCEEDED(request->get_Headers(&reqHeaders)) && reqHeaders &&
                            SUCCEEDED(reqHeaders->GetHeader(L"If-None-Match", &match)) && match) {
                            if (wcscmp(match, etag) == 0) {
                                status = 304;
                                reason = L"Not Modified";
                            }
                            CoTaskMemFree(match);
                        }
                        if (status != 304 && !ReadWholeFile(assetPath.c_str(), body)) {
                            extraHeaders.clear();
                            cacheControl = L"no-store";
                            mime = L"text/plain; charset=utf-8";
                            body = "Asset not found";
                            status = 404;
                            reason = L"Not Found";
                        }
                    } else {
                        body = "Asset not found";
                        status = 404;
                        reason = L"Not Found";
                    }
                } else if (path.rfind(L"lib/", 0) == 0) {
                    std::wstring libPath;
                    const std::string* cached = nullptr;
                    if (ResolveUnderRoot(L"lib", path.substr(4), libPath) &&
                        (cached = LoadLibCached(libPath)) != nullptr) {
                        payload = cached;
                        mimeOwned = MimeForPath(libPath);
                        mime = mimeOwned.c_str();
                        cacheControl = L"max-age=86400";
                    } else {
                        body = "Library not found";
                        status = 404;
                        reason = L"Not Found";
                    }
                } else if (path.rfind(L"themes/", 0) == 0) {
                    std::wstring themePath;
                    if (ResolveUnderRoot(L"themes", path.substr(7), themePath) &&
                        ReadWholeFile(themePath.c_str(), body)) {
                        mimeOwned = MimeForPath(themePath);
                        mime = mimeOwned.c_str();
                    } else {
                        body = "Theme not found";
                        status = 404;
                        reason = L"Not Found";
                    }
                } else {
                    payload = &LoadIndexHtml();
                    mime = L"text/html; charset=utf-8";
                }

                ComPtr<ICoreWebView2WebResourceResponse> response;
                if (SUCCEEDED(CreateResponse(g_environment.Get(), *payload, mime,
                        &response, status, reason, cacheControl, extraHeaders))) {
                    args->put_Response(response.Get());
                }
                return S_OK;
            }).Get(), &token);
}

// --- WebView2 lifecycle -----------------------------------------------------------

void ResizeWebView()
{
    if (g_app.controller && g_app.hwnd) {
        RECT rc = {};
        GetClientRect(g_app.hwnd, &rc);
        g_app.controller->put_Bounds(rc);
    }
    if (g_placeholder && g_app.hwnd) {
        RECT rc = {};
        GetClientRect(g_app.hwnd, &rc);
        MoveWindow(g_placeholder, 0, rc.bottom / 2 - 18, rc.right, 36, TRUE);
    }
}

void SetPlaceholderText(const wchar_t* text)
{
    if (g_placeholder) {
        SetWindowTextW(g_placeholder, text);
        ShowWindow(g_placeholder, SW_SHOW);
    }
}

void CreateController()
{
    g_environment->CreateCoreWebView2Controller(g_app.hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                if (FAILED(hr) || !controller) {
                    SetPlaceholderText(L"WebView2 failed to create a controller.");
                    return S_OK;
                }
                g_app.controller = controller;
                g_app.controller->get_CoreWebView2(&g_app.webview);
                ResizeWebView();

                EventRegistrationToken accelToken = {};
                g_app.controller->add_AcceleratorKeyPressed(
                    Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                        [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                            COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
                            UINT key = 0;
                            args->get_KeyEventKind(&kind);
                            args->get_VirtualKey(&key);
                            const bool down = (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                                               kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN);
                            if (!down || g_app.printing) {
                                return S_OK;
                            }
                            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                            const wchar_t* action = nullptr;
                            if (key == VK_F1) {
                                action = L"help";
                            } else if (key == VK_ESCAPE) {
                                action = L"escape";
                            } else if (key == VK_F7) {
                                action = L"f7";
                            } else if (key == VK_F5) {
                                action = L"present";  // also blocks browser refresh
                            } else if (ctrl && shift && !alt && key == 'S') {
                                action = L"saveas";
                            } else if (ctrl && !shift && !alt) {
                                switch (key) {
                                case 'S': action = L"save"; break;
                                case 'O': action = L"open"; break;
                                case 'E': action = L"split"; break;
                                case 'D': action = L"theme"; break;
                                case 'T': action = L"toc"; break;
                                case 'G': action = L"top"; break;
                                case 'L': action = L"linenumbers"; break;
                                case 'H': action = L"replace"; break;
                                case 'K': action = L"quickopen"; break;
                                case 'R': action = L"noop"; break;  // swallow reload: edits would be lost
                                case 'Q': action = L"syncsplit"; break;
                                }
                            }
                            if (action && g_app.webview) {
                                args->put_Handled(TRUE);
                                std::wstring msg = L"{\"key\":\"";
                                msg += action;
                                msg += L"\"}";
                                g_app.webview->PostWebMessageAsJson(msg.c_str());
                            }
                            return S_OK;
                        }).Get(), &accelToken);

                EventRegistrationToken navToken = {};
                g_app.webview->add_NavigationCompleted(
                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                            if (g_placeholder) {
                                ShowWindow(g_placeholder, SW_HIDE);
                            }
                            return S_OK;
                        }).Get(), &navToken);

                EventRegistrationToken frameNavToken = {};
                g_app.webview->add_FrameNavigationStarting(
                    Callback<ICoreWebView2NavigationStartingEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                            LPWSTR rawUri = nullptr;
                            args->get_Uri(&rawUri);
                            std::wstring uri = rawUri ? rawUri : L"";
                            CoTaskMemFree(rawUri);
                            if (uri.rfind(kOrigin, 0) == 0 ||
                                uri.rfind(L"about:", 0) == 0 ||
                                uri.rfind(L"data:", 0) == 0) {
                                return S_OK;
                            }
                            args->put_Cancel(TRUE);
                            if (uri.rfind(L"http://", 0) == 0 || uri.rfind(L"https://", 0) == 0) {
                                ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                            }
                            return S_OK;
                        }).Get(), &frameNavToken);

                EventRegistrationToken messageToken = {};
                g_app.webview->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR rawSource = nullptr;
                            args->get_Source(&rawSource);
                            const bool trusted = rawSource && wcscmp(rawSource, kOrigin) == 0;
                            CoTaskMemFree(rawSource);
                            if (!trusted) {
                                return S_OK;
                            }
                            LPWSTR raw = nullptr;
                            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                std::wstring message(raw);
                                CoTaskMemFree(raw);
                                if (message.rfind(L"dropfiles|", 0) == 0) {
                                    // First dropped file travels as an AdditionalObject.
                                    std::wstring dropped;
                                    ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
                                    ComPtr<ICoreWebView2ObjectCollectionView> objects;
                                    if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))) &&
                                        SUCCEEDED(args2->get_AdditionalObjects(&objects)) && objects) {
                                        UINT32 count = 0;
                                        objects->get_Count(&count);
                                        for (UINT32 i = 0; i < count && dropped.empty(); ++i) {
                                            ComPtr<IUnknown> item;
                                            if (FAILED(objects->GetValueAtIndex(i, &item)) || !item) {
                                                continue;
                                            }
                                            ComPtr<ICoreWebView2File> file;
                                            if (SUCCEEDED(item.As(&file))) {
                                                LPWSTR path = nullptr;
                                                if (SUCCEEDED(file->get_Path(&path)) && path) {
                                                    dropped = path;
                                                    CoTaskMemFree(path);
                                                }
                                            }
                                        }
                                    }
                                    HandleDropMessage(message, dropped);
                                } else {
                                    HandleWebMessage(message);
                                }
                            }
                            return S_OK;
                        }).Get(), &messageToken);

                RegisterResourceHandler();
                g_app.webview->Navigate(kOrigin);
                return S_OK;
            }).Get());
}

std::wstring ResolveUserDataFolder()
{
    LoadSettings();
    if (g_settings.cacheMode == L"custom" && !g_settings.cachePath.empty()) {
        SHCreateDirectoryExW(nullptr, g_settings.cachePath.c_str(), nullptr);
        if (DirectoryWritable(g_settings.cachePath)) {
            return g_settings.cachePath;
        }
    }
    wchar_t* base = nullptr;
    size_t count = 0;
    _wdupenv_s(&base, &count, L"LOCALAPPDATA");
    std::wstring dir = base ? std::wstring(base) + L"\\JAMDedit\\WebView2"
                            : ModuleDirectory() + L"\\webview2_data";
    if (base) {
        free(base);
    }
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

void BootstrapWebView()
{
    const std::wstring userDataFolder = ResolveUserDataFolder();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || !environment) {
                    SetPlaceholderText(
                        L"WebView2 Runtime is not available.\r\nInstall Microsoft Edge WebView2 Runtime.");
                    return S_OK;
                }
                g_environment = environment;
                CreateController();
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        SetPlaceholderText(L"WebView2 Runtime is not available.");
    }
}

// --- window ------------------------------------------------------------------------

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_SIZE:
        ResizeWebView();
        return 0;

    case WM_SETFOCUS:
        if (g_app.controller) {
            g_app.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        return 0;

    case WM_SETTINGCHANGE:
        ApplyTitlebarTheme(hwnd);
        return 0;

    case WM_TIMER:
        if (wparam == 1) {
            CheckExternalChange();
        }
        return 0;

    case kMsgLinkCheck: {
        std::unique_ptr<std::wstring> payload(reinterpret_cast<std::wstring*>(lparam));
        if (payload && static_cast<unsigned>(wparam) == g_linkCheckGen.load()) {
            PostJson(*payload);
        }
        return 0;
    }

    case WM_CLOSE:
        if (g_app.forceClose || !g_app.webview) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (g_app.closeQueryPending) {
            // Second close while waiting on the page: assume it's wedged.
            DestroyWindow(hwnd);
            return 0;
        }
        if (!g_app.dirty) {
            DestroyWindow(hwnd);
            return 0;
        }
        // Ask the page for the current text so unsaved changes can be written.
        g_app.closeQueryPending = true;
        PostJson(L"{\"closequery\":true}");
        return 0;

    case WM_DESTROY: {
        // Persist window size for next launch.
        WINDOWPLACEMENT wp = { sizeof(wp) };
        if (GetWindowPlacement(hwnd, &wp) && wp.showCmd == SW_SHOWNORMAL) {
            RECT& rc = wp.rcNormalPosition;
            WriteIni(L"WindowW", std::to_wstring(rc.right - rc.left));
            WriteIni(L"WindowH", std::to_wstring(rc.bottom - rc.top));
        }
        if (g_app.controller) {
            g_app.controller->Close();
            g_app.webview.Reset();
            g_app.controller.Reset();
        }
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int showCmd)
{
    g_instance = instance;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }

    LoadSettings();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    const int width = (std::max)(640, g_settings.winW);
    const int height = (std::max)(480, g_settings.winH);
    HWND hwnd = CreateWindowExW(0, kWindowClassName, L"JAMD Editor",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        return 1;
    }
    g_app.hwnd = hwnd;
    ApplyTitlebarTheme(hwnd);

    g_placeholder = CreateWindowExW(0, L"STATIC", L"Starting WebView2...",
        WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 10, 10, hwnd, nullptr, instance, nullptr);

    // Command line: a single (optionally quoted) file path.
    std::wstring fileArg;
    if (cmdLine && *cmdLine) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
        if (argv) {
            if (argc >= 1) {
                fileArg = argv[0];
            }
            LocalFree(argv);
        }
    }
    if (!fileArg.empty()) {
        std::string bytes;
        if (ReadWholeFile(fileArg.c_str(), bytes)) {
            wchar_t full[MAX_PATH * 4] = {};
            if (GetFullPathNameW(fileArg.c_str(), ARRAYSIZE(full), full, nullptr)) {
                SetDocument(full, std::move(bytes));
            } else {
                SetDocument(fileArg, std::move(bytes));
            }
        }
    }
    UpdateTitle();

    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);
    ResizeWebView();
    BootstrapWebView();
    SetTimer(hwnd, 1, 2000, nullptr);  // external-change watcher

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
