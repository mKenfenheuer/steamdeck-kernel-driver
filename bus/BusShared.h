//
// BusShared.h - protocol shared between SteamDeckBus.sys and SteamDeckHID.sys
//
// Both drivers must agree on:
//   - the bus device name (so HID can IoGetDeviceObjectPointer it)
//   - the SUBMIT IOCTL code
//   - the wire format of the XUSB report
//
// Keep this file dependency-free: only ntdef.h / wdm.h primitives.
//
#pragma once

//
// Named device path of the bus FDO. Created by SteamDeckBus.sys when its FDO
// starts and torn down at unload. The HID driver opens this by name with
// IoGetDeviceObjectPointer; no symlink, no usermode access.
//
#define SDBUS_NT_DEVICE_NAME_W      L"\\Device\\SteamDeckBus"
#define SDBUS_NT_DEVICE_NAME_UNICODE { sizeof(SDBUS_NT_DEVICE_NAME_W) - sizeof(WCHAR), \
                                      sizeof(SDBUS_NT_DEVICE_NAME_W),                 \
                                      SDBUS_NT_DEVICE_NAME_W }

//
// Submit IOCTL. METHOD_BUFFERED + FILE_WRITE_DATA so a kernel caller can
// allocate a small input buffer and let the IO manager copy it to the bus.
// Reused IOCTL function code 0x800 (start of vendor-defined range).
//
#define IOCTL_SDBUS_SUBMIT_XUSB \
    CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)

//
// Wire-format XUSB report. Mirrors XINPUT_GAMEPAD exactly. Packed so
// it's identical on both sides regardless of compiler defaults.
//
#pragma pack(push, 1)
typedef struct _SDBUS_XUSB_REPORT
{
    USHORT  wButtons;       // XUSB_GAMEPAD_* bitfield
    UCHAR   bLeftTrigger;   // 0..255
    UCHAR   bRightTrigger;  // 0..255
    SHORT   sThumbLX;       // -32768..32767
    SHORT   sThumbLY;       // -32768..32767
    SHORT   sThumbRX;
    SHORT   sThumbRY;
} SDBUS_XUSB_REPORT, *PSDBUS_XUSB_REPORT;
#pragma pack(pop)

//
// XUSB button bits (must match XINPUT_GAMEPAD_*; see XInput.h).
//
#define SDBUS_XUSB_DPAD_UP          0x0001
#define SDBUS_XUSB_DPAD_DOWN        0x0002
#define SDBUS_XUSB_DPAD_LEFT        0x0004
#define SDBUS_XUSB_DPAD_RIGHT       0x0008
#define SDBUS_XUSB_START            0x0010
#define SDBUS_XUSB_BACK             0x0020
#define SDBUS_XUSB_LEFT_THUMB       0x0040
#define SDBUS_XUSB_RIGHT_THUMB      0x0080
#define SDBUS_XUSB_LEFT_SHOULDER    0x0100
#define SDBUS_XUSB_RIGHT_SHOULDER   0x0200
#define SDBUS_XUSB_GUIDE            0x0400
#define SDBUS_XUSB_A                0x1000
#define SDBUS_XUSB_B                0x2000
#define SDBUS_XUSB_X                0x4000
#define SDBUS_XUSB_Y                0x8000
