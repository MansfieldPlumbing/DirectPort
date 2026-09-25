// =============================================================================
// Menu.h  --  Custom-drawn tray menu (Mica Alt, dark, rounded)
// =============================================================================
// A small popup-menu implementation matching Windows 11 styling.  Ownership is simple: the caller
// news the top-level menu and Show()s it; it deletes itself when its window
// is destroyed.  Submenus are owned by their parent item.
// =============================================================================

#pragma once

#include <windows.h>
#include <memory>
#include <string>
#include <vector>

// WM_APP message the menu sends to its owner window: wParam = command id.
constexpr UINT WM_APP_MENU_COMMAND = WM_APP + 2;

class PopupMenu {
public:
    PopupMenu(HWND owner, HINSTANCE instance);
    ~PopupMenu();

    void AddItem(const std::wstring& text, UINT id, bool checked = false, bool enabled = true);
    void AddSeparator();
    PopupMenu* AddSubMenu(const std::wstring& text);

    // Shows the menu anchored at a screen point (flipped to stay on-screen).
    void ShowAt(POINT anchor);

    static void CloseAll();
    static bool IsOpen();

private:
    struct Item {
        std::wstring text;
        UINT id = 0;
        bool separator = false;
        bool checked = false;
        bool enabled = true;
        std::unique_ptr<PopupMenu> subMenu;
    };

    void Show(int x, int y);
    int Width() const;
    int Height() const;
    int ItemHeight(const Item& item) const;
    RECT ItemRect(size_t index) const;
    int HitTest(POINT client) const;
    void Hover(int index);
    void OpenSubMenu(int index);
    void CloseSubMenu();
    void Paint(HDC hdc);

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    HINSTANCE m_instance = nullptr;
    PopupMenu* m_parent = nullptr;
    PopupMenu* m_openSub = nullptr;
    std::vector<Item> m_items;
    int m_hover = -1;
    int m_dpi = 96;
    mutable int m_width = 0;
};
