#include "shell_icons.h"

#include <ole2.h>  // WIN32_LEAN_AND_MEAN leaves out what exdisp.h needs
#include <exdisp.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <set>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace {

double Now() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / f.QuadPart;
}

enum class Kind { None, Folder, Desktop, Apps };

Kind KindOf(HWND h, const std::wstring& cls) {
    if (cls == L"CabinetWClass") return Kind::Folder;
    if (cls == L"Progman" || cls == L"WorkerW") return Kind::Desktop;
    if (cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd") return Kind::Apps;
    if (cls == L"Windows.UI.Core.CoreWindow") {
        wchar_t title[32] = {};
        GetWindowTextW(h, title, 32);
        if (!wcscmp(title, L"Start")) return Kind::Apps;
    }
    return Kind::None;
}

// Taskbar buttons are named "Microsoft Edge - 2 running windows pinned",
// "Microsoft Store pinned" or "Notepad - 1 running window".
std::wstring AppName(std::wstring name) {
    const std::wstring pinned = L" pinned";
    if (name.size() > pinned.size() && name.compare(name.size() - pinned.size(), pinned.size(), pinned) == 0)
        name.resize(name.size() - pinned.size());
    const size_t dash = name.rfind(L" - ");
    if (dash != std::wstring::npos && name.find(L"running window", dash) != std::wstring::npos) name.resize(dash);
    return name;
}

std::wstring Key(Kind k, HWND h, const std::wstring& name) {
    if (k == Kind::Apps) return L"apps|" + AppName(name);
    wchar_t w[32];
    swprintf_s(w, L"w%p|", (void*)h);
    return w + name;
}

// A shell item's icon at size x size, premultiplied.
std::shared_ptr<IconImage> LoadIconAt(IShellItemImageFactory* factory, int size) {
    HBITMAP bmp = nullptr;
    if (FAILED(factory->GetImage({size, size}, SIIGBF_ICONONLY, &bmp)) || !bmp) return nullptr;
    BITMAP bm = {};
    GetObjectW(bmp, sizeof(bm), &bm);
    auto img = std::make_shared<IconImage>();
    img->width = bm.bmWidth;
    img->height = std::abs(bm.bmHeight);
    img->pixels.resize((size_t)img->width * img->height);
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), img->width, -img->height, 1, 32, BI_RGB};
    HDC dc = GetDC(nullptr);
    const int rows = GetDIBits(dc, bmp, 0, img->height, img->pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(bmp);
    if (rows != img->height || img->width < 4) return nullptr;
    // Straight or premultiplied alpha, or none at all?
    bool anyAlpha = false, straight = false;
    for (uint32_t p : img->pixels) {
        const uint32_t a = p >> 24;
        anyAlpha |= a != 0;
        straight |= ((p >> 16) & 0xFF) > a || ((p >> 8) & 0xFF) > a || (p & 0xFF) > a;
    }
    for (uint32_t& p : img->pixels) {
        if (!anyAlpha) {
            p |= 0xFF000000u;
        } else if (straight) {
            const uint32_t a = p >> 24;
            auto mul = [a](uint32_t c) { return (c * a + 127) / 255; };
            p = (a << 24) | (mul((p >> 16) & 0xFF) << 16) | (mul((p >> 8) & 0xFF) << 8) | mul(p & 0xFF);
        }
    }
    return img;
}

// The large icon of a shell item, with its small versions.
std::shared_ptr<IconImage> LoadIcon(IShellItem* item) {
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&factory)))) return nullptr;
    auto big = LoadIconAt(factory.Get(), 256);
    if (!big) return nullptr;
    const UINT dpi = GetDpiForSystem();
    for (int s0 : {16, 20, 24, 32, 40, 48, 64}) {
        const int s = MulDiv(s0, dpi, 96);
        if (!big->atSize.count(s))
            if (auto one = LoadIconAt(factory.Get(), s); one && one->width == s && one->height == s)
                big->atSize[s] = one;
    }
    return big;
}

// The shell view of an Explorer window, or of the desktop.
ComPtr<IFolderView> ViewOf(HWND h, bool desktop) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return nullptr;
    ComPtr<IDispatch> disp;
    if (desktop) {
        VARIANT loc, empty;
        VariantInit(&loc);
        VariantInit(&empty);
        loc.vt = VT_I4;
        loc.lVal = CSIDL_DESKTOP;
        long hw = 0;
        windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hw, SWFO_NEEDDISPATCH, &disp);
    } else {
        long count = 0;
        windows->get_Count(&count);
        for (long i = 0; i < count && !disp; ++i) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = i;
            ComPtr<IDispatch> d;
            ComPtr<IWebBrowserApp> app;
            SHANDLE_PTR hw = 0;
            if (SUCCEEDED(windows->Item(v, &d)) && d && SUCCEEDED(d.As(&app)) && SUCCEEDED(app->get_HWND(&hw)) &&
                (HWND)hw == h)
                disp = d;
        }
    }
    ComPtr<IServiceProvider> sp;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView> fv;
    if (!disp || FAILED(disp.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&fv)))
        return nullptr;
    return fv;
}

struct ViewedFolder {
    std::wstring path;
    std::map<std::wstring, ComPtr<IShellItem>> items;  // by the name the view shows
    double at = -1e9;
};

// The folder a view shows and its items.
bool ReadFolder(IFolderView* fv, ViewedFolder& out) {
    ComPtr<IShellFolder> sf;
    ComPtr<IPersistFolder2> pf;
    PIDLIST_ABSOLUTE folder = nullptr;
    if (FAILED(fv->GetFolder(IID_PPV_ARGS(&sf))) || FAILED(fv->GetFolder(IID_PPV_ARGS(&pf))) ||
        FAILED(pf->GetCurFolder(&folder)))
        return false;
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(folder, SIGDN_DESKTOPABSOLUTEPARSING, &path)) && path) out.path = path;
    CoTaskMemFree(path);
    CoTaskMemFree(folder);
    out.items.clear();
    int n = 0;
    fv->ItemCount(SVGIO_ALLVIEW, &n);
    for (int i = 0; i < (std::min)(n, 3000); ++i) {
        PITEMID_CHILD child = nullptr;
        if (FAILED(fv->Item(i, &child)) || !child) continue;
        STRRET sr, pr;
        wchar_t name[MAX_PATH] = {}, parsing[MAX_PATH] = {};
        ComPtr<IShellItem> item;
        if (SUCCEEDED(sf->GetDisplayNameOf(child, SHGDN_INFOLDER | SHGDN_NORMAL, &sr)) &&
            SUCCEEDED(StrRetToBufW(&sr, child, name, MAX_PATH))) {
            // From the full path where there is one: an item made from the
            // view's own id can lose what kind of item it is (and get the
            // blank document icon).
            if (SUCCEEDED(sf->GetDisplayNameOf(child, SHGDN_FORPARSING, &pr)) &&
                SUCCEEDED(StrRetToBufW(&pr, child, parsing, MAX_PATH)))
                SHCreateItemFromParsingName(parsing, nullptr, IID_PPV_ARGS(&item));
            if (!item) SHCreateItemWithParent(nullptr, sf.Get(), child, IID_PPV_ARGS(&item));
            if (item) out.items[name] = item;
        }
        CoTaskMemFree(child);
    }
    return true;
}

// Installed apps (the Start menu's "All apps"), by name.
void ReadApps(std::map<std::wstring, ComPtr<IShellItem>>& apps) {
    apps.clear();
    ComPtr<IShellItem> folder;
    ComPtr<IEnumShellItems> items;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder))) ||
        FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items))))
        return;
    ComPtr<IShellItem> item;
    while (items->Next(1, &item, nullptr) == S_OK) {
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) apps[name] = item;
        CoTaskMemFree(name);
        item.Reset();
    }
}

}  // namespace

void ShellIcons::Start() {
    m_stop = false;
    m_thread = std::thread([this] { Loop(); });
}

void ShellIcons::Stop() {
    {
        std::lock_guard l(m_mu);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

std::shared_ptr<const IconImage> ShellIcons::Find(HWND hwnd, const std::wstring& cls, const std::wstring& name) {
    const Kind kind = KindOf(hwnd, cls);
    if (kind == Kind::None || name.empty()) return nullptr;
    const std::wstring key = Key(kind, hwnd, name);
    std::lock_guard l(m_mu);
    if (auto it = m_icons.find(key); it != m_icons.end()) return it->second;
    if (auto it = m_missing.find(key); it != m_missing.end() && Now() - it->second < 30) return nullptr;
    m_missing[key] = Now();  // pending: asked once until it is loaded or 30 s pass
    m_wanted.push_back({hwnd, name});
    m_cv.notify_all();
    return nullptr;
}

void ShellIcons::Loop() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        std::map<std::wstring, ComPtr<IShellItem>> apps;
        double appsAt = -1e9;
        std::map<HWND, ViewedFolder> folders;
        int loaded = 0;
        while (true) {
            std::vector<std::pair<HWND, std::wstring>> todo;
            {
                std::unique_lock l(m_mu);
                m_cv.wait(l, [&] { return m_stop || !m_wanted.empty(); });
                if (m_stop) break;
                todo.swap(m_wanted);
            }
            for (const auto& [h, name] : todo) {
                wchar_t cls[64] = {};
                GetClassNameW(h, cls, 64);
                const Kind kind = KindOf(h, cls);
                const std::wstring key = Key(kind, h, name);
                ComPtr<IShellItem> item;
                if (kind == Kind::Apps) {
                    if (Now() - appsAt > 600) ReadApps(apps), appsAt = Now();
                    if (auto it = apps.find(AppName(name)); it != apps.end()) item = it->second;
                } else if (kind == Kind::Folder || kind == Kind::Desktop) {
                    ViewedFolder& fo = folders[h];
                    if (Now() - fo.at > 2) {
                        const std::wstring before = fo.path;
                        auto fv = ViewOf(h, kind == Kind::Desktop);
                        if (fv) ReadFolder(fv.Get(), fo);
                        fo.at = Now();
                        if (fo.path != before) {
                            // Another folder: icons by these names may differ now.
                            wchar_t w[32];
                            swprintf_s(w, L"w%p|", (void*)h);
                            std::lock_guard l(m_mu);
                            for (auto it = m_icons.begin(); it != m_icons.end();)
                                it = it->first.rfind(w, 0) == 0 ? m_icons.erase(it) : std::next(it);
                        }
                    }
                    if (auto it = fo.items.find(name); it != fo.items.end()) item = it->second;
                }
                std::shared_ptr<IconImage> img = item ? LoadIcon(item.Get()) : nullptr;
                std::lock_guard l(m_mu);
                if (img) {
                    m_icons[key] = img;
                    m_missing.erase(key);
                    if (++loaded <= 3) Log(L"Icon loaded: %dx%d", img->width, img->height);
                } else {
                    m_missing[key] = Now();
                }
            }
        }
    }
    CoUninitialize();
}
