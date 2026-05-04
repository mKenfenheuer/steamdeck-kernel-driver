#pragma once

//
// SteamDeckHID – Native KMDF HID Minidriver
// Driver.h  –  Master header included by every translation unit.
//
// Defines:
//   - Hardware constants (VID/PID, report IDs, interface indices)
//   - All packed raw/translated report structs
//   - Per-device context (DEVICE_CONTEXT)
//   - Forward declarations of every public function
//

// ── Kernel / WDF headers ────────────────────────────────────────────────────
// We are a plain USB function driver now (no mshidkmdf above us, no HID
// minidriver hat); reports are forwarded to SteamDeckBus over the kernel
// IPC channel only. So no <hidport.h> here.
#include <ntddk.h>
#include <wdf.h>
#include <usb.h>        // Must precede wdfusb.h (defines USB_REQUEST_*, USBD_STATUS, etc.)
#include <usbdlib.h>    // Must precede wdfusb.h (USBD_VERSION_INFORMATION, PURB, etc.)
#include <usbioctl.h>
#include <wdfusb.h>

// SteamDeckBus IOCTL + wire format (bus driver in ../bus/)
#include "BusShared.h"

// ── Compiler helpers ─────────────────────────────────────────────────────────
#ifndef FORCEINLINE
#define FORCEINLINE __forceinline
#endif

// ── Pool tagging (helps WinDbg !poolfind) ────────────────────────────────────
#define STEAMDECK_POOL_TAG  'KDCS'   // 'SCDK' reversed

// =============================================================================
//  HARDWARE CONSTANTS  –  Valve Neptune / Steam Deck
// =============================================================================

#define NEPTUNE_VID                             0x28DE
#define NEPTUNE_PID                             0x1205

// USB interface indices within the composite device
#define NEPTUNE_IFACE_KEYBOARD                  0   // bInterfaceProtocol = 1
#define NEPTUNE_IFACE_MOUSE                     1   // bInterfaceProtocol = 2
#define NEPTUNE_IFACE_GAMEPAD                   2   // bInterfaceProtocol = 0

// USB interrupt IN packet size for the gamepad interface
#define NEPTUNE_INPUT_REPORT_SIZE               64

// HID report IDs
#define NEPTUNE_REPORT_ID_INPUT                 0x01

// Neptune feature-report IDs (Valve packet types). Cross-referenced with
// neptune-hidapi.net (Hid/HidEnums.cs::SDCPacketType) and SetLizardMode().
#define NEPTUNE_PT_CLEAR_MAPPINGS               0x81  // disable button emulation
#define NEPTUNE_PT_LIZARD_BUTTONS               0x85  // ENABLE button emulation
#define NEPTUNE_PT_CONFIGURE                    0x87  // configure (used to disable mouse)
#define NEPTUNE_PT_LIZARD_MOUSE                 0x8E  // ENABLE mouse emulation

// =============================================================================
//  NEPTUNE RAW INPUT REPORT  (60 wire bytes, Report ID = 0x01, padded to 64)
//
//  Layout taken verbatim from neptune-hidapi.net (Hid/HidEnums.cs::SDCInput).
//  C# uses LayoutKind.Sequential with default packing -> a single byte of
//  alignment padding sits between Buttons5 (offset 0x0E) and LeftPadX
//  (offset 0x10). The C struct below is `pack(1)` and inserts that padding
//  explicitly so offsets line up with the wire format byte-for-byte.
// =============================================================================

#pragma pack(push, 1)
typedef struct _NEPTUNE_RAW_REPORT
{
    UCHAR   PacketType;         // 0x00  must be 0x01 for input reports
    UCHAR   Reserved0[3];       // 0x01
    ULONG   Sequence;           // 0x04  monotonic counter
    USHORT  Buttons0;           // 0x08  see NEPTUNE_BTN0_*
    UCHAR   Buttons1;           // 0x0A  see NEPTUNE_BTN1_*
    UCHAR   Buttons2;           // 0x0B  see NEPTUNE_BTN2_*
    UCHAR   Buttons3;           // 0x0C  (unused in known firmware)
    UCHAR   Buttons4;           // 0x0D  see NEPTUNE_BTN4_*
    UCHAR   Buttons5;           // 0x0E  see NEPTUNE_BTN5_*
    UCHAR   Padding0;           // 0x0F  alignment to match C# Sequential layout
    SHORT   LeftPadX;           // 0x10
    SHORT   LeftPadY;           // 0x12
    SHORT   RightPadX;          // 0x14
    SHORT   RightPadY;          // 0x16
    SHORT   AccelX;             // 0x18
    SHORT   AccelY;             // 0x1A
    SHORT   AccelZ;             // 0x1C
    SHORT   GyroPitch;          // 0x1E
    SHORT   GyroYaw;            // 0x20
    SHORT   GyroRoll;           // 0x22
    SHORT   Q1;                 // 0x24
    SHORT   Q2;                 // 0x26
    SHORT   Q3;                 // 0x28
    SHORT   Q4;                 // 0x2A
    SHORT   LeftTrigger;        // 0x2C  0..32767 (15-bit unsigned in INT16)
    SHORT   RightTrigger;       // 0x2E
    SHORT   LeftStickX;         // 0x30  -32768..32767  (left = negative)
    SHORT   LeftStickY;         // 0x32  -32768..32767  (up   = positive)
    SHORT   RightStickX;        // 0x34
    SHORT   RightStickY;        // 0x36
    SHORT   LeftPadPressure;    // 0x38
    SHORT   RightPadPressure;   // 0x3A
} NEPTUNE_RAW_REPORT, *PNEPTUNE_RAW_REPORT;
#pragma pack(pop)

C_ASSERT(sizeof(NEPTUNE_RAW_REPORT) == 0x3C);
C_ASSERT(sizeof(NEPTUNE_RAW_REPORT) <= NEPTUNE_INPUT_REPORT_SIZE);

// =============================================================================
//  NEPTUNE BUTTON BITMASKS
//  Source: neptune-hidapi.net Hid/HidEnums.cs (SDCButton0..5).
// =============================================================================

// Buttons0 (USHORT, little-endian on the wire)
#define NEPTUNE_BTN0_R2             0x0001
#define NEPTUNE_BTN0_L2             0x0002
#define NEPTUNE_BTN0_R1             0x0004
#define NEPTUNE_BTN0_L1             0x0008
#define NEPTUNE_BTN0_Y              0x0010
#define NEPTUNE_BTN0_B              0x0020
#define NEPTUNE_BTN0_X              0x0040
#define NEPTUNE_BTN0_A              0x0080
#define NEPTUNE_BTN0_DPAD_UP        0x0100
#define NEPTUNE_BTN0_DPAD_RIGHT     0x0200
#define NEPTUNE_BTN0_DPAD_LEFT      0x0400
#define NEPTUNE_BTN0_DPAD_DOWN      0x0800
#define NEPTUNE_BTN0_MENU           0x1000  // hamburger / Start
#define NEPTUNE_BTN0_STEAM          0x2000  // Steam / Guide
#define NEPTUNE_BTN0_OPTIONS        0x4000  // ellipsis / Back / Select
#define NEPTUNE_BTN0_L5             0x8000  // back paddle left inner

// Buttons1 (UCHAR)
#define NEPTUNE_BTN1_R5             0x01    // back paddle right inner
#define NEPTUNE_BTN1_LPAD_PRESS     0x02
#define NEPTUNE_BTN1_RPAD_TOUCH     0x04
#define NEPTUNE_BTN1_LPAD_TOUCH     0x08
#define NEPTUNE_BTN1_RPAD_PRESS     0x10
#define NEPTUNE_BTN1_LSTICK_PRESS   0x40

// Buttons2 (UCHAR)
#define NEPTUNE_BTN2_RSTICK_PRESS   0x04

// Buttons4 (UCHAR)
#define NEPTUNE_BTN4_L4             0x02    // back paddle left outer
#define NEPTUNE_BTN4_R4             0x04    // back paddle right outer
#define NEPTUNE_BTN4_LSTICK_TOUCH   0x40
#define NEPTUNE_BTN4_RSTICK_TOUCH   0x80

// Buttons5 (UCHAR)
#define NEPTUNE_BTN5_QUICK_ACCESS   0x04    // QAM / "..."

// =============================================================================
//  STEAMDECK HID INPUT REPORT  –  what we expose to Windows
//  Must match the Report Descriptor in Hid.c byte-for-byte.
// =============================================================================

#pragma pack(push, 1)
typedef struct _STEAMDECK_HID_INPUT_REPORT
{
    UCHAR   ReportId;       // always 0x01
    USHORT  Buttons;        // 16 buttons, 1 bit each (LSB = Button 1 = A)
    UCHAR   Hat;            // D-Pad: lower nibble = hat (0-7, 8=neutral),
                            //        upper nibble = padding
    CHAR    LeftStickX;     // -127..127
    CHAR    LeftStickY;     // -127..127
    CHAR    RightStickX;
    CHAR    RightStickY;
    UCHAR   LeftTrigger;    // 0..255
    UCHAR   RightTrigger;
} STEAMDECK_HID_INPUT_REPORT, *PSTREAMDECK_HID_INPUT_REPORT;
#pragma pack(pop)

#define HID_INPUT_REPORT_SIZE  sizeof(STEAMDECK_HID_INPUT_REPORT)

// =============================================================================
//  DEVICE CONTEXT  –  one instance per device node
// =============================================================================

typedef struct _DEVICE_CONTEXT
{
    WDFDEVICE           WdfDevice;

    // USB
    WDFUSBDEVICE        UsbDevice;
    WDFUSBINTERFACE     UsbInterface;       // interface 2 (gamepad)
    WDFUSBPIPE          InterruptInPipe;

    // Lock guarding the chord state machine + LizardModeEnabled.
    KSPIN_LOCK          ReportLock;

    // Whether the WDF continuous reader is currently active
    BOOLEAN             ReaderActive;

    // Periodic 250 ms timer that re-asserts the current Lizard-mode state
    // (disabled by default; enabled while LizardModeEnabled is TRUE).
    // The timer runs at DISPATCH and just enqueues the WORKITEM, which
    // runs at PASSIVE_LEVEL to issue the synchronous USB control transfer.
    WDFTIMER            LizardTimer;
    WDFWORKITEM         LizardWorkItem;

    // User-controlled "muted" state. Toggled by holding L3+R3 (HID buttons
    // 9 and 10) for 3 seconds. When TRUE:
    //   * The work item ENABLES Lizard mode on the device (firmware does
    //     keyboard/mouse emulation; MI_00/MI_01 are nulled in the INF, so
    //     Windows sees nothing from those interfaces).
    //   * The HID gamepad report we publish is forced to neutral so games
    //     don't see spurious input.
    // When FALSE: normal gamepad operation, Lizard kept disabled.
    BOOLEAN             LizardModeEnabled;

    // L3+R3 chord state machine. Updated under ReportLock from ReadComplete.
    // ChordStartTime is interrupt-time (100 ns units) when both buttons were
    // first observed pressed; 0 means the chord is not currently held.
    // ChordConsumed is set after the 3 s threshold fires, and cleared when
    // the buttons are released, so each hold toggles only once.
    ULONGLONG           ChordStartTime;
    BOOLEAN             ChordConsumed;

    // -- SteamDeckBus link --------------------------------------------------
    // Cached referenced pointer to the bus driver's named device object,
    // resolved lazily on the first ReadComplete after start. NULL means the
    // bus isn't loaded yet -- ReadComplete keeps trying without spamming.
    PDEVICE_OBJECT      BusDevice;
    PFILE_OBJECT        BusFileObject;
    LONG                BusResolveAttempted;   // 0/1 atomic flag

    // Last XUSB report we sent. Lets us skip identical resubmits across the
    // bus IRP; matches the bus's own no-op fast-path but saves the IRP build.
    SDBUS_XUSB_REPORT   LastXusb;
    KSPIN_LOCK          XusbLock;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

// =============================================================================
//  FUNCTION PROTOTYPES
// =============================================================================

// Driver.c
DRIVER_INITIALIZE       DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SteamDeckHID_EvtDeviceAdd;

// Device.c
NTSTATUS SteamDeckDevice_Create(
    _In_         PWDFDEVICE_INIT   DeviceInit,
    _Outptr_opt_ WDFDEVICE        *Device);

EVT_WDF_DEVICE_PREPARE_HARDWARE SteamDeckDevice_EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SteamDeckDevice_EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY         SteamDeckDevice_EvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT          SteamDeckDevice_EvtD0Exit;

// Usb.c
NTSTATUS SteamDeckUsb_Init(_In_ WDFDEVICE Device);
NTSTATUS SteamDeckUsb_DisableLizardMode(_In_ WDFDEVICE Device);
NTSTATUS SteamDeckUsb_EnableLizardMode(_In_ WDFDEVICE Device);
NTSTATUS SteamDeckUsb_StartReader(_In_ WDFDEVICE Device);
VOID     SteamDeckUsb_StopReader(_In_ WDFDEVICE Device);

// Periodic 250 ms timer that keeps re-sending the Lizard disable commands.
NTSTATUS SteamDeckUsb_CreateLizardTimer(_In_ WDFDEVICE Device);
VOID     SteamDeckUsb_StartLizardTimer(_In_ WDFDEVICE Device);
VOID     SteamDeckUsb_StopLizardTimer(_In_ WDFDEVICE Device);

EVT_WDF_USB_READER_COMPLETION_ROUTINE SteamDeckUsb_ReadComplete;
EVT_WDF_USB_READERS_FAILED            SteamDeckUsb_ReadError;
EVT_WDF_TIMER                         SteamDeckUsb_LizardTimerFunc;

// Report.c
VOID SteamDeckReport_Translate(
    _In_  PNEPTUNE_RAW_REPORT          Raw,
    _Out_ PSTREAMDECK_HID_INPUT_REPORT Hid);

// XInput.c - submits the translated report to the bus driver's virtual X360.
VOID SteamDeckXInput_Translate(
    _In_  PSTREAMDECK_HID_INPUT_REPORT Hid,
    _Out_ PSDBUS_XUSB_REPORT           Xusb);

VOID SteamDeckXInput_Submit(
    _In_ WDFDEVICE         Device,
    _In_ PSDBUS_XUSB_REPORT Report);

// Resolves \Device\SteamDeckBus and caches the pointer in DEVICE_CONTEXT.
// MUST be called at PASSIVE_LEVEL (typically from EvtPrepareHardware).
VOID SteamDeckXInput_Resolve(_In_ WDFDEVICE Device);

VOID SteamDeckXInput_Cleanup(_In_ WDFDEVICE Device);
