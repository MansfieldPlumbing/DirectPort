#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <string>
#include <sstream>
#include <vector>
#include <new>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

// {1A2B3C4D-5E6F-7A8B-9C0D-1E2F3A4B5C6D}
const CLSID CLSID_MenuGraphDumper = 
{ 0x1A2B3C4D, 0x5E6F, 0x7A8B, { 0x9C, 0x0D, 0x1E, 0x2F, 0x3A, 0x4B, 0x5C, 0x6D } };

const wchar_t* CLSID_STR = L"{1A2B3C4D-5E6F-7A8B-9C0D-1E2F3A4B5C6D}";

HINSTANCE g_hInst = NULL;
long g_cRefModule = 0;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hInst = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

// -----------------------------------------------------------------------------
// Clipboard & Tree Walking
// -----------------------------------------------------------------------------

void CopyToClipboard(const std::wstring& text) {
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        size_t size = (text.length() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
        if (hMem) {
            memcpy(GlobalLock(hMem), text.c_str(), size);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
    }
}

void WalkMenuGraph(HMENU hMenu, int depth, std::wstringstream& ss) {
    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; i++) {
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_STRING;
        
        wchar_t textBuf[512] = { 0 };
        mii.dwTypeData = textBuf;
        mii.cch = ARRAYSIZE(textBuf);

        if (GetMenuItemInfoW(hMenu, i, TRUE, &mii)) {
            for (int d = 0; d < depth; d++) ss << L"    ";

            if (mii.fType & MFT_SEPARATOR) {
                ss << L"|-- [SEPARATOR]\n";
            } else {
                ss << L"|-- ";
                if (mii.cch > 0 && textBuf[0] != L'\0') {
                    ss << L"\"" << textBuf << L"\"";
                } else if (mii.fType & MFT_OWNERDRAW) {
                    ss << L"[OWNER DRAWN ITEM - No String]";
                } else {
                    ss << L"[EMPTY STRING]";
                }
                
                ss << L" (CmdID: " << mii.wID;
                if (mii.fState & MFS_DISABLED) ss << L", DISABLED";
                if (mii.fState & MFS_CHECKED)  ss << L", CHECKED";
                ss << L")\n";
            }

            if (mii.hSubMenu != NULL) {
                WalkMenuGraph(mii.hSubMenu, depth + 1, ss);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// COM Object
// -----------------------------------------------------------------------------

class CMenuGraphDumper : public IShellExtInit, public IContextMenu {
private:
    long m_cRef;
public:
    CMenuGraphDumper() : m_cRef(1) { InterlockedIncrement(&g_cRefModule); }
    ~CMenuGraphDumper() { InterlockedDecrement(&g_cRefModule); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IContextMenu)) {
            *ppv = static_cast<IContextMenu*>(this);
        } else if (IsEqualIID(riid, IID_IShellExtInit)) {
            *ppv = static_cast<IShellExtInit*>(this);
        } else {
            *ppv = NULL; return E_NOINTERFACE;
        }
        AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_cRef); }
    IFACEMETHODIMP_(ULONG) Release() {
        ULONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0) delete this;
        return cRef;
    }

    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject*, HKEY) { return S_OK; }

    IFACEMETHODIMP QueryContextMenu(HMENU hmenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags) {
        if (uFlags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

        std::wstringstream ss;
        ss << L"CONTEXT MENU GRAPH DUMP:\n";
        ss << L"========================\n";
        WalkMenuGraph(hmenu, 0, ss);
        ss << L"========================\n";
        
        CopyToClipboard(ss.str());

        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0); 
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO*) { return S_OK; }
    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, LPSTR, UINT) { return E_NOTIMPL; }
};

// -----------------------------------------------------------------------------
// COM Boilerplate & Registration
// -----------------------------------------------------------------------------

class CClassFactory : public IClassFactory {
private:
    long m_cRef;
public:
    CClassFactory() : m_cRef(1) { InterlockedIncrement(&g_cRefModule); }
    ~CClassFactory() { InterlockedDecrement(&g_cRefModule); }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
        }
        *ppv = NULL; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_cRef); }
    IFACEMETHODIMP_(ULONG) Release() {
        ULONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0) delete this; return cRef;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        CMenuGraphDumper* pExt = new (std::nothrow) CMenuGraphDumper();
        if (!pExt) return E_OUTOFMEMORY;
        HRESULT hr = pExt->QueryInterface(riid, ppv);
        pExt->Release(); return hr;
    }
    IFACEMETHODIMP LockServer(BOOL fLock) {
        if (fLock) InterlockedIncrement(&g_cRefModule);
        else InterlockedDecrement(&g_cRefModule);
        return S_OK;
    }
};

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (IsEqualCLSID(CLSID_MenuGraphDumper, rclsid)) {
        CClassFactory* pFactory = new (std::nothrow) CClassFactory();
        if (!pFactory) return E_OUTOFMEMORY;
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release(); return hr;
    }
    *ppv = NULL; return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() { return (g_cRefModule == 0) ? S_OK : S_FALSE; }

// --- Registration Helpers ---
HRESULT SetHKCUKeyAndValue(PCWSTR pszSubKey, PCWSTR pszValueName, PCWSTR pszData) {
    HKEY hKey = NULL;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, pszSubKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (status == ERROR_SUCCESS) {
        if (pszData != NULL) {
            status = RegSetValueExW(hKey, pszValueName, 0, REG_SZ, (const BYTE*)pszData, (lstrlenW(pszData) + 1) * sizeof(WCHAR));
        }
        RegCloseKey(hKey);
    }
    return HRESULT_FROM_WIN32(status);
}

// Called by regsvr32.exe /s
STDAPI DllRegisterServer() {
    wchar_t szModule[MAX_PATH];
    if (GetModuleFileNameW(g_hInst, szModule, ARRAYSIZE(szModule)) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + CLSID_STR;
    
    // 1. Register COM Object
    SetHKCUKeyAndValue(clsidKey.c_str(), NULL, L"Menu Graph Dumper");
    SetHKCUKeyAndValue((clsidKey + L"\\InProcServer32").c_str(), NULL, szModule);
    SetHKCUKeyAndValue((clsidKey + L"\\InProcServer32").c_str(), L"ThreadingModel", L"Apartment");

    // 2. Register Context Menu Handler for All Files (*) and Directory Backgrounds
    SetHKCUKeyAndValue(L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\MenuGraphDumper", NULL, CLSID_STR);
    SetHKCUKeyAndValue(L"Software\\Classes\\Directory\\Background\\shellex\\ContextMenuHandlers\\MenuGraphDumper", NULL, CLSID_STR);

    return S_OK;
}

// Called by regsvr32.exe /u /s
STDAPI DllUnregisterServer() {
    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + CLSID_STR;
    
    RegDeleteTreeW(HKEY_CURRENT_USER, clsidKey.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\MenuGraphDumper");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shellex\\ContextMenuHandlers\\MenuGraphDumper");

    return S_OK;
}