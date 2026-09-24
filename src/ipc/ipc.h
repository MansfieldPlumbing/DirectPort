#pragma once
#include <windows.h>

#pragma pack(push, 1)
struct MenuElement {
    UINT commandId;
    UINT flags;
    INT parentIndex;
    BOOL hasChildren;
    wchar_t label[128];
};

struct MenuPayload {
    POINT cursorPosition;
    UINT itemCount;
    LONG selectedCommandId; // (Keeping for struct sizing)
    MenuElement elements[256];
};
#pragma pack(pop)