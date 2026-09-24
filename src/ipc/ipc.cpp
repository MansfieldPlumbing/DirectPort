#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <new>
#include <string>
#include <sddl.h>
#include "ipc.h"

const CLSID CLSID_MenuGraphDumper = { 0x1A2B3C4D, 0x5E6F, 0x7A8B, { 0x9C, 0x0D, 0x1E, 0x2F, 0x3A, 0x4B, 0x5C, 0x6D } };
const wchar_t* CLSID_STR = L"{1A2B3C4D-5E6F-7A8B-9C0D-1E2F3A4B5C6D}";

HINSTANCE g_hInst = NULL;
long g_cRefModule = 0;

// Win32 Shared Memory states
HANDLE g_hSharedMem = NULL;
MenuPayload* g_pPayload = NULL;
HANDLE g_hPushEvent = NULL;
HANDLE g_hResultEvent = NULL;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) { 
        g_hInst = hModule; 
        DisableThreadLibraryCalls(hModule); 
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (g_pPayload) UnmapViewOfFile(g_pPayload);
        if (g_hSharedMem) CloseHandle(g_hSharedMem);
        if (g_hPushEvent) CloseHandle(g_hPushEvent);
        if (g_hResultEvent) CloseHandle(g_hResultEvent);
    }
    return TRUE;
}

void WalkMenuGraph(HMENU hMenu, int parentIndex, MenuPayload* payload) {
    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; i++) {
        if (payload->itemCount >= 256) break;

        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_STRING;
        wchar_t textBuf[128] = { 0 };
        mii.dwTypeData = textBuf;
        mii.cch = ARRAYSIZE(textBuf);

        if (GetMenuItemInfoW(hMenu, i, TRUE, &mii)) {
            int myIndex = payload->itemCount++;
            MenuElement& el = payload->elements[myIndex];
            
            el.commandId = mii.wID;
            el.flags = mii.fState;
            el.parentIndex = parentIndex;
            el.hasChildren = (mii.hSubMenu != NULL);
            
            if (mii.cch > 0 && textBuf[0] != L'\0') {
                wcsncpy_s(el.label, _countof(el.label), textBuf, _TRUNCATE);
            } else if (mii.fType & MFT_SEPARATOR) {
                wcscpy_s(el.label, _countof(el.label), L"[SEPARATOR]");
            } else {
                wcscpy_s(el.label, _countof(el.label), L"[EMPTY]");
            }

            if (mii.hSubMenu != NULL) WalkMenuGraph(mii.hSubMenu, myIndex, payload);
        }
    }
}

class CMenuGraphDumper : public IShellExtInit, public IContextMenu3 {
private:
    long m_cRef;
    UINT m_idCmdFirst;
    MenuPayload m_localPayload;

public:
    CMenuGraphDumper() : m_cRef(1), m_idCmdFirst(0) { InterlockedIncrement(&g_cRefModule); }
    ~CMenuGraphDumper() { InterlockedDecrement(&g_cRefModule); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IContextMenu) || IsEqualIID(riid, IID_IContextMenu2) || IsEqualIID(riid, IID_IContextMenu3)) {
            *ppv = static_cast<IContextMenu3*>(this);
        } else if (IsEqualIID(riid, IID_IShellExtInit)) { *ppv = static_cast<IShellExtInit*>(this); }
        else { *ppv = NULL; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_cRef); }
    
    IFACEMETHODIMP_(ULONG) Release() { ULONG cRef = InterlockedDecrement(&m_cRef); if (cRef == 0) delete this; return cRef; }
    
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject*, HKEY) { return S_OK; }

    IFACEMETHODIMP QueryContextMenu(HMENU hmenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags) {
        if (uFlags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

        // Initialize pure Win32 IPC (No D3D12 loaded into Explorer!)
        if (!g_hSharedMem) {
            PSECURITY_DESCRIPTOR sd = nullptr;
            ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL);
            SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };

            g_hSharedMem = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(MenuPayload), L"Local\\Explorer_Menu_Payload");
            g_pPayload = (MenuPayload*)MapViewOfFile(g_hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MenuPayload));
            g_hPushEvent = CreateEventW(&sa, FALSE, FALSE, L"Local\\Explorer_Menu_Push");
            g_hResultEvent = CreateEventW(&sa, FALSE, FALSE, L"Local\\Explorer_Menu_Result");
            if (sd) LocalFree(sd);
        }

        m_idCmdFirst = idCmdFirst;

        // Build payload locally
        memset(&m_localPayload, 0, sizeof(MenuPayload));
        GetCursorPos(&m_localPayload.cursorPosition);
        WalkMenuGraph(hmenu, -1, &m_localPayload);

        // Strip native menu
        int count = GetMenuItemCount(hmenu);
        for (int i = count - 1; i >= 0; i--) RemoveMenu(hmenu, i, MF_BYPOSITION);

        // Insert invisible trigger item
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_FTYPE | MIIM_ID;
        mii.fType = MFT_OWNERDRAW; 
        mii.wID = idCmdFirst;
        InsertMenuItemW(hmenu, 0, TRUE, &mii);

        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 1); 
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* pici) { return S_OK; }
    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, LPSTR, UINT) { return E_NOTIMPL; }
    IFACEMETHODIMP HandleMenuMsg(UINT uMsg, WPARAM wParam, LPARAM lParam) { LRESULT res; return HandleMenuMsg2(uMsg, wParam, lParam, &res); }

    IFACEMETHODIMP HandleMenuMsg2(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* plResult) {
        if (!plResult) return E_POINTER; *plResult = 0;

        if (uMsg == WM_MEASUREITEM && g_pPayload) {
            LPMEASUREITEMSTRUCT lpmis = (LPMEASUREITEMSTRUCT)lParam;
            lpmis->itemWidth = 0; lpmis->itemHeight = 0;
            
            // 1. Push memory out of Explorer immediately
            memcpy(g_pPayload, &m_localPayload, sizeof(MenuPayload));
            
            // 2. Signal the Adapter
            SetEvent(g_hPushEvent);

            // 3. Wait for the Adapter to release us (with a 5 second safety timeout to prevent Explorer hangs)
            WaitForSingleObject(g_hResultEvent, 5000);

            EndMenu(); 
            *plResult = TRUE;
            return S_OK;
        }
        return S_FALSE; 
    }
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

STDAPI DllRegisterServer() {
    wchar_t szModule[MAX_PATH];
    if (GetModuleFileNameW(g_hInst, szModule, ARRAYSIZE(szModule)) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + CLSID_STR;
    
    SetHKCUKeyAndValue(clsidKey.c_str(), NULL, L"Menu Graph Dumper");
    SetHKCUKeyAndValue((clsidKey + L"\\InProcServer32").c_str(), NULL, szModule);
    SetHKCUKeyAndValue((clsidKey + L"\\InProcServer32").c_str(), L"ThreadingModel", L"Apartment");

    SetHKCUKeyAndValue(L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\MenuGraphDumper", NULL, CLSID_STR);
    SetHKCUKeyAndValue(L"Software\\Classes\\Directory\\Background\\shellex\\ContextMenuHandlers\\MenuGraphDumper", NULL, CLSID_STR);

    return S_OK;
}

STDAPI DllUnregisterServer() {
    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + CLSID_STR;
    RegDeleteTreeW(HKEY_CURRENT_USER, clsidKey.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\MenuGraphDumper");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shellex\\ContextMenuHandlers\\MenuGraphDumper");
    return S_OK;
}