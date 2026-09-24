#pragma once
/*
    WASAPI.abi.h — Record.Sexe ABI ledger

    This file is DATA for Record.ps1. It is never compiled or #included.
    Record.ps1 parses only ABI_CONST_*, ABI_GUID_* and ABI_SLOT_* defines.

    Provenance
      Windows SDK contracts:
        mmdeviceapi.h
        audioclient.h
        propkeydef.h / functiondiscoverykeys_devpkey.h
      Working native cross-check:
        MansfieldPlumbing/VirtuaCam
        commit 398b7ce4fe4afc3b7012aa912dfc75ce69f5d8ae
        src/VirtuaCam/WASAPI.h
        src/VirtuaCam/WASAPI.cpp

    The slot numbers include the three inherited IUnknown entries:
      QueryInterface=0, AddRef=1, Release=2.
*/

/* Constants consumed by Record.ps1. */
#define ABI_CONST_COINIT_MULTITHREADED               0x00000000
#define ABI_CONST_RPC_E_CHANGED_MODE                 0x80010106
#define ABI_CONST_CLSCTX_ALL                         0x00000017
#define ABI_CONST_DEVICE_STATE_ACTIVE                0x00000001
#define ABI_CONST_eRender                            0
#define ABI_CONST_eCapture                           1
#define ABI_CONST_STGM_READ                          0x00000000
#define ABI_CONST_PKEY_Device_FriendlyName_PID       14
#define ABI_CONST_AUDCLNT_SHAREMODE_SHARED           0
#define ABI_CONST_AUDCLNT_STREAMFLAGS_LOOPBACK       0x00020000
#define ABI_CONST_AUDCLNT_BUFFERFLAGS_SILENT         0x00000002
#define ABI_CONST_WAVE_FORMAT_PCM                    0x0001
#define ABI_CONST_WAVE_FORMAT_IEEE_FLOAT             0x0003
#define ABI_CONST_WAVE_FORMAT_EXTENSIBLE             0xFFFE

/* GUID strings consumed as System.Guid values by Record.ps1. */
#define ABI_GUID_CLSID_MMDeviceEnumerator            "BCDE0395-E52F-467C-8E3D-C4579291692E"
#define ABI_GUID_IID_IMMDeviceEnumerator             "A95664D2-9614-4F35-A746-DE8DB63617E6"
#define ABI_GUID_IID_IAudioClient                    "1CB9AD4C-DBFA-4C32-B178-C2F568A703B2"
#define ABI_GUID_IID_IAudioCaptureClient             "C8ADBD64-E71E-48A0-A4DE-185C395CD317"
#define ABI_GUID_PKEY_Device_FriendlyName_FMTID       "A45C254E-DF1C-4EFD-8020-67D146A850E0"

/* COM vtable slots actually touched by Record.ps1. */
#define ABI_SLOT_IUnknown_Release                     2

#define ABI_SLOT_IMMDeviceEnumerator_EnumAudioEndpoints 3
#define ABI_SLOT_IMMDeviceEnumerator_GetDevice          5

#define ABI_SLOT_IMMDeviceCollection_GetCount         3
#define ABI_SLOT_IMMDeviceCollection_Item             4

#define ABI_SLOT_IMMDevice_Activate                   3
#define ABI_SLOT_IMMDevice_OpenPropertyStore          4
#define ABI_SLOT_IMMDevice_GetId                      5

#define ABI_SLOT_IPropertyStore_GetValue              5

#define ABI_SLOT_IAudioClient_Initialize              3
#define ABI_SLOT_IAudioClient_GetMixFormat            8
#define ABI_SLOT_IAudioClient_Start                  10
#define ABI_SLOT_IAudioClient_Stop                   11
#define ABI_SLOT_IAudioClient_GetService             14

#define ABI_SLOT_IAudioCaptureClient_GetBuffer        3
#define ABI_SLOT_IAudioCaptureClient_ReleaseBuffer    4
#define ABI_SLOT_IAudioCaptureClient_GetNextPacketSize 5
