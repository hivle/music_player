#include "platform.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <shobjidl.h>
    #include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace platform {

#ifdef _WIN32

// UTF-8 -> UTF-16.
static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// UTF-16 -> UTF-8.
static std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::string pick_folder(const std::string& initial_dir) {
    std::string result;

    // CoInitializeEx may fail with RPC_E_CHANGED_MODE if the host already
    // initialised apartment-threaded COM; treat that as "already ready".
    HRESULT hr_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool need_uninit = SUCCEEDED(hr_init);

    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Select music folder");

        if (!initial_dir.empty()) {
            IShellItem* item = nullptr;
            std::wstring winit = to_wide(initial_dir);
            if (SUCCEEDED(SHCreateItemFromParsingName(winit.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                dlg->SetFolder(item);
                item->Release();
            }
        }

        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = to_utf8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }

    if (need_uninit) CoUninitialize();
    return result;
}

std::vector<std::string> find_ui_fonts() {
    std::vector<std::string> out;
    const char* win = std::getenv("WINDIR");
    std::string root = (win && *win) ? std::string(win) + "\\Fonts\\" : std::string("C:\\Windows\\Fonts\\");
    const char* candidates[] = {
        "seguiemj.ttf",  // Segoe UI Emoji — not great for text but broad
        "msyh.ttc",      // Microsoft YaHei (Simplified Chinese)
        "msyh.ttf",
        "YuGothM.ttc",   // Yu Gothic Medium (Japanese)
        "meiryo.ttc",    // Meiryo (Japanese)
        "malgun.ttf",    // Malgun Gothic (Korean)
        "simsun.ttc",    // SimSun (Simplified Chinese)
        "segoeui.ttf",   // Segoe UI (Latin/Cyrillic/Greek/Arabic/Hebrew)
        "arial.ttf",
    };
    for (const char* c : candidates) {
        std::string p = root + c;
        std::error_code ec;
        if (fs::exists(p, ec)) out.push_back(p);
    }
    return out;
}

#elif defined(__APPLE__)

static std::string run_pipe(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string pick_folder(const std::string& /*initial_dir*/) {
    // osascript returns the POSIX path of the chosen folder, or errors on cancel.
    return run_pipe("osascript -e 'try' -e 'POSIX path of (choose folder with prompt \"Select music folder\")' -e 'on error' -e 'return \"\"' -e 'end try' 2>/dev/null");
}

std::vector<std::string> find_ui_fonts() {
    std::vector<std::string> out;
    const char* candidates[] = {
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/System/Library/Fonts/AppleSDGothicNeo.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial Unicode.ttf",
    };
    for (const char* c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) out.push_back(c);
    }
    return out;
}

#else // Linux / other POSIX

static std::string run_pipe(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int rc = pclose(p);
    if (rc != 0) return "";  // non-zero exit => user cancelled / tool missing
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static bool has_tool(const char* name) {
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

static std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string pick_folder(const std::string& initial_dir) {
    std::string dir = initial_dir;
    if (dir.empty()) {
        std::error_code ec;
        dir = fs::current_path(ec).string();
    }
    std::string qdir = shell_quote(dir);

    if (has_tool("zenity")) {
        return run_pipe("zenity --file-selection --directory --title='Select music folder' --filename=" + qdir + "/ 2>/dev/null");
    }
    if (has_tool("kdialog")) {
        return run_pipe("kdialog --getexistingdirectory " + qdir + " --title 'Select music folder' 2>/dev/null");
    }
    if (has_tool("yad")) {
        return run_pipe("yad --file --directory --title='Select music folder' --filename=" + qdir + "/ 2>/dev/null");
    }
    return "";
}

std::vector<std::string> find_ui_fonts() {
    std::vector<std::string> out;
    const char* candidates[] = {
        // Broad-coverage CJK fonts first so Asian tags render.
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/wqy-microhei/wqy-microhei.ttc",
        "/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        // Latin + Cyrillic + Greek fallbacks.
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    };
    for (const char* c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) out.push_back(c);
    }
    return out;
}

#endif

} // namespace platform
