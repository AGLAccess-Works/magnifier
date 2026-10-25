// Large icons from Windows for what is on screen: files and folders in
// Explorer windows and on the desktop, and apps in the Start menu and on the
// taskbar. Windows keeps most of these at up to 256x256 pixels, so the
// magnifier can draw them sharp instead of enlarging the 16-48 pixel copies
// on screen, as text is redrawn from its font.
//
// Shell calls run on a thread of their own (single-threaded apartment, as
// some shell extensions require); lookups never wait for them: an icon not
// yet loaded is requested and is there by a later scan.
#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// An icon image: premultiplied BGRA, top-down rows. The large image is what
// is drawn; atSize holds Windows' own artwork at the usual on-screen sizes
// (often a simpler design than the large one shrunk), to find the icon on
// screen by.
struct IconImage {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;
    std::map<int, std::shared_ptr<const IconImage>> atSize;  // small versions, by size
};

class ShellIcons {
public:
    void Start();
    void Stop();

    // The icon for an item named name in top-level window hwnd (of window
    // class cls), or nullptr if none is known (yet).
    std::shared_ptr<const IconImage> Find(HWND hwnd, const std::wstring& cls, const std::wstring& name);

private:
    void Loop();

    std::thread m_thread;
    std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_stop = false;
    std::vector<std::pair<HWND, std::wstring>> m_wanted;  // window, item name
    // Loaded icons: by window (Explorer, desktop) or "apps", then item name.
    std::map<std::wstring, std::shared_ptr<const IconImage>> m_icons;
    std::map<std::wstring, double> m_missing;  // looked for and not found: when
};
