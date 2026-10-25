#include "cursor_hide.h"

#include <magnification.h>
#include <shellapi.h>

#include <atomic>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"

namespace cursorhide {
namespace {

struct SystemPointer {
    UINT id;                 // OCR_* value
    const wchar_t* regName;  // value under HKCU\Control Panel\Cursors
    HCURSOR shared = nullptr;  // the system's shared handle (what GetCursorInfo reports)
    HCURSOR copy = nullptr;    // our copy of the original image
};

SystemPointer g_pointers[] = {
    {32512, L"Arrow"},    {32513, L"IBeam"},       {32514, L"Wait"},     {32515, L"Crosshair"},
    {32516, L"UpArrow"},  {32642, L"SizeNWSE"},    {32643, L"SizeNESW"}, {32644, L"SizeWE"},
    {32645, L"SizeNS"},   {32646, L"SizeAll"},     {32648, L"No"},       {32649, L"Hand"},
    {32650, L"AppStarting"}, {32651, L"Help"},     {32671, L"Pin"},      {32672, L"Person"},
};

std::mutex g_mu;  // guards g_pointers' handles: the render thread reads them
std::atomic<uint64_t> g_generation{0};
bool g_hidden = false;
bool g_mag = false;  // MagShowSystemCursor available (UIAccess)
int g_reasserts = 0;

void Restore() { SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0); }

// Call with g_mu held.
const SystemPointer* Find(HCURSOR c) {
    for (const auto& p : g_pointers)
        if (c && p.shared == c) return &p;
    return nullptr;
}

constexpr int kBlankSize = 32;
constexpr uint32_t kBlankPixel = 0x01000000;  // premultiplied black, alpha 1

// A 32-bit pointer that is invisible to the eye but not empty: one pixel at
// 1/255 opacity. Some Remote Desktop clients mishandle 1-bit pointers and
// keep drawing the previous shape, and some draw their own arrow for an
// entirely empty one; either way the real pointer stayed visible there.
HCURSOR MakeBlank() {
    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = kBlankSize;
    bh.bV5Height = -kBlankSize;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    memset(bits, 0, (size_t)kBlankSize * kBlankSize * 4);
    static_cast<uint32_t*>(bits)[0] = kBlankPixel;
    BYTE maskBits[kBlankSize * kBlankSize / 8];
    memset(maskBits, 0xFF, sizeof(maskBits));
    HBITMAP mask = CreateBitmap(kBlankSize, kBlankSize, 1, 1, maskBits);
    ICONINFO ii = {FALSE, 0, 0, mask, color};
    HCURSOR c = mask ? (HCURSOR)CreateIconIndirect(&ii) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return c;
}

// Our blanks: an all-ones AND mask and transparent black but for one pixel.
bool IsBlank(HCURSOR c) {
    ICONINFO ii = {};
    if (!c || !GetIconInfo(c, &ii)) return false;
    bool blank = ii.hbmColor && ii.hbmMask;
    if (blank) {
        BITMAP bm = {}, cm = {};
        GetObjectW(ii.hbmMask, sizeof(bm), &bm);
        GetObjectW(ii.hbmColor, sizeof(cm), &cm);
        std::vector<BYTE> mbits((size_t)bm.bmWidthBytes * bm.bmHeight), cbits((size_t)cm.bmWidthBytes * cm.bmHeight);
        if (bm.bmBitsPixel != 1 || cm.bmBitsPixel != 32 || mbits.empty() || cbits.empty() ||
            !GetBitmapBits(ii.hbmMask, (LONG)mbits.size(), mbits.data()) ||
            !GetBitmapBits(ii.hbmColor, (LONG)cbits.size(), cbits.data())) {
            blank = false;
        } else {
            for (size_t i = 0; i < mbits.size() && blank; ++i) blank = mbits[i] == 0xFF;
            // At most the one faint pixel (Windows may store the rows flipped).
            int faint = 0;
            for (size_t i = 0; i < cbits.size() && blank; ++i)
                if (cbits[i]) blank = cbits[i] == 1 && ++faint == 1;
        }
    }
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    return blank;
}

void Snapshot() {
    {
        std::lock_guard lock(g_mu);
        for (auto& p : g_pointers) {
            if (p.copy) DestroyCursor(p.copy);
            p.shared = LoadCursorW(nullptr, MAKEINTRESOURCEW(p.id));
            p.copy = p.shared ? CopyIcon(p.shared) : nullptr;
        }
    }
    ++g_generation;
}

void Blank() {
    int replaced = 0;
    for (const auto& p : g_pointers) {
        if (!p.shared) continue;  // only Snapshot (this thread) writes it
        // SetSystemCursor takes ownership, so each id needs its own blank.
        HCURSOR blank = MakeBlank();
        if (blank && SetSystemCursor(blank, p.id)) ++replaced;
        else if (blank) DestroyCursor(blank);
    }
    if (g_mag) MagShowSystemCursor(FALSE);
    // A zero-distance move makes the window under the pointer set it again,
    // so a Remote Desktop client is sent the new shape straight away.
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
    Log(L"Pointer hidden (%d of %d system pointers replaced%s)", replaced, (int)std::size(g_pointers),
        g_mag ? L", all pointers hidden by the Magnification API" : L"");
}

}  // namespace

void Init() {
    Restore();
    Snapshot();
}

void UseMagnificationApi() {
    g_mag = MagInitialize() != FALSE;
    if (!g_mag) Log(L"Magnification API unavailable (%u); hiding standard pointers only", GetLastError());
}

void Hide() {
    if (g_hidden) return;
    Blank();
    g_hidden = true;
}

void Show() {
    if (!g_hidden) return;
    if (g_mag) MagShowSystemCursor(TRUE);
    Restore();
    g_hidden = false;
    Log(L"Pointer restored");
}

void Reassert() {
    if (!g_hidden) return;
    // Diagnostics: a pointer that is still visible, and where.
    static int reported = 0;
    static HCURSOR lastReported = nullptr;
    CURSORINFO ci = {sizeof(ci)};
    if (reported < 30 && GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor &&
        ci.hCursor != lastReported && !IsBlank(ci.hCursor)) {
        lastReported = ci.hCursor;
        ++reported;
        wchar_t cls[64] = L"";
        GetClassNameW(WindowFromPoint(ci.ptScreenPos), cls, 64);
        const wchar_t* name = L"app pointer";
        {
            std::lock_guard lock(g_mu);
            if (const SystemPointer* p = Find(ci.hCursor)) name = p->regName;
        }
        Log(L"Visible pointer: %s %p over %s", name, ci.hCursor, cls);
    }
    if (IsBlank(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))) && IsBlank(LoadCursorW(nullptr, MAKEINTRESOURCEW(32513))))
        return;
    // Windows loaded the scheme again: these are the user's current pointers.
    if (++g_reasserts <= 20) Log(L"Pointers were restored by Windows; hiding them again");
    Snapshot();
    Blank();
}

HCURSOR CopyOriginal(HCURSOR cursor) {
    std::lock_guard lock(g_mu);
    const SystemPointer* p = Find(cursor);
    return CopyIcon(p && p->copy ? p->copy : cursor);
}

uint64_t Generation() { return g_generation; }

HCURSOR LoadLarge(HCURSOR cursor, int size) {
    UINT id = 0;
    const wchar_t* regName = nullptr;
    {
        std::lock_guard lock(g_mu);
        if (const SystemPointer* p = Find(cursor)) id = p->id, regName = p->regName;
    }
    if (!id) {
        // Application pointer: ask Windows to redraw it from its resource.
        return (HCURSOR)CopyImage(cursor, IMAGE_CURSOR, size, size, LR_COPYFROMRESOURCE);
    }
    // The user's scheme (including the Windows pointer size/colour settings)
    // names a .cur/.ani file per pointer; an empty value means the default.
    wchar_t path[MAX_PATH] = {}, expanded[MAX_PATH] = {};
    DWORD bytes = sizeof(path);
    HCURSOR big = nullptr;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", regName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     nullptr, path, &bytes) == ERROR_SUCCESS &&
        path[0]) {
        const DWORD n = ExpandEnvironmentStringsW(path, expanded, MAX_PATH);
        if (n && n <= MAX_PATH) big = (HCURSOR)LoadImageW(nullptr, expanded, IMAGE_CURSOR, size, size, LR_LOADFROMFILE);
    }
    if (!big) big = (HCURSOR)LoadImageW(nullptr, MAKEINTRESOURCEW(id), IMAGE_CURSOR, size, size, 0);
    return big;
}

void StartWatchdog() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --watchdog " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return;
    }
    DWORD err = GetLastError();
    if (err == ERROR_ELEVATION_REQUIRED) {
        // A uiAccess exe can refuse CreateProcess; ShellExecute goes through
        // the AppInfo service, which is allowed to start it.
        std::wstring params = L"--watchdog " + std::to_wstring(GetCurrentProcessId());
        SHELLEXECUTEINFOW sei = {sizeof(sei)};
        sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        sei.lpFile = exe;
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei)) return;
        err = GetLastError();
    }
    Log(L"Could not start the pointer watchdog: %u", err);
}

int RunWatchdog(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    Restore();
    return 0;
}

}  // namespace cursorhide
