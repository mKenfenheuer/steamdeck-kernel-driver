//
// BusDriver.h - master header for SteamDeckBus.sys
//
// Architecture:
//   [SteamDeckHID.sys]  -- IOCTL_SDBUS_SUBMIT_XUSB -->  [Bus FDO]
//                                                            |
//                                              child list (1 PDO)
//                                                            |
//                                       [XUSB PDO  USB\VID_045E&PID_028E]
//                                                            |
//                                                       xusb22.sys
//                                                            |
//                                                       XInput games
//
// The FDO is root-enumerated (one node, named "\Device\SteamDeckBus").
// The PDO is auto-spawned at FDO PrepareHardware and never removed -- the
// device "exists" for the bus's lifetime. xusb22 attaches once and stays.
//
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbioctl.h>
#include <usbbusif.h>
#include <wdmsec.h>

#include "BusShared.h"

#define SDBUS_POOL_TAG          'BDCS'   // 'SCDB'

// Default Microsoft Xbox 360 wired identifiers (overridable via INF/registry).
#define SDBUS_DEFAULT_VID       0x045E
#define SDBUS_DEFAULT_PID       0x028E

// The single child target. Serial is irrelevant (no usermode plug/unplug),
// but the child list machinery wants something stable to compare against.
#define SDBUS_FIXED_SERIAL      1

// =============================================================================
//  CHILD LIST IDENTIFICATION DESCRIPTION
//
//  Required by WDF child list. Single-PDO bus only ever has one of these.
// =============================================================================
typedef struct _SDBUS_PDO_IDENTIFICATION_DESCRIPTION
{
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Header;
    ULONG  SerialNo;
    USHORT VendorId;
    USHORT ProductId;
} SDBUS_PDO_IDENTIFICATION_DESCRIPTION, *PSDBUS_PDO_IDENTIFICATION_DESCRIPTION;

// =============================================================================
//  FDO CONTEXT
// =============================================================================
typedef struct _SDBUS_FDO_CONTEXT
{
    WDFDEVICE   Device;

    // Default VID/PID applied to the spawned child PDO; loaded from the
    // service Parameters key at DriverEntry, falling back to MS Xbox 360.
    USHORT      DefaultVendorId;
    USHORT      DefaultProductId;

    // Set after the child PDO has been added so a SUBMIT IOCTL can find it
    // in the child list without iterating an empty list.
    BOOLEAN     ChildPlugged;
} SDBUS_FDO_CONTEXT, *PSDBUS_FDO_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SDBUS_FDO_CONTEXT, SdBusFdoGetContext)

// =============================================================================
//  PDO CONTEXT
//
//  All XUSB state lives here. ViGEm spread this across an EmulationTargetXUSB
//  C++ class - we flatten it into a POD struct with file-local helpers.
// =============================================================================

// XUSB interrupt-IN packet xusb22 expects on the wire: 0x14 (20) bytes total.
//   [0]    Id    = 0x00
//   [1]    Size  = 0x14  (xusb22 keys off this byte, not the URB length)
//   [2..13] XUSB_REPORT (12 bytes)
//   [14..19] zero padding
//
// ViGEm's XUSB_INTERRUPT_IN_PACKET is only 14 bytes -- it works because
// xusb22 stops reading at TransferBufferLength -- but we model the full
// 20-byte protocol shape so memcpy from the boot blob (also 20 bytes)
// lines up cleanly without underflow.
#pragma pack(push, 1)
typedef struct _SDBUS_XUSB_PACKET
{
    UCHAR              Id;
    UCHAR              Size;
    SDBUS_XUSB_REPORT  Report;
    UCHAR              Pad[6];
} SDBUS_XUSB_PACKET, *PSDBUS_XUSB_PACKET;
#pragma pack(pop)

C_ASSERT(sizeof(SDBUS_XUSB_PACKET) == 0x14);

typedef struct _SDBUS_PDO_CONTEXT
{
    WDFDEVICE   PdoDevice;
    WDFDEVICE   ParentFdo;     // the FDO that owns us

    USHORT      VendorId;
    USHORT      ProductId;
    ULONG       SerialNo;

    // Last submitted XUSB packet. ReadComplete copies this into the pending
    // IN URB transfer buffer when xusb22 polls.
    SDBUS_XUSB_PACKET  Packet;
    KSPIN_LOCK         PacketLock;

    // Manual queue holding xusb22's IN URBs until SubmitReport completes them.
    WDFQUEUE    PendingDataIn;

    // Manual queue holding control-pipe IN URBs (xusb22's auxiliary endpoint).
    WDFQUEUE    PendingCtlIn;

    // xusb22 expects a "boot sequence" of 6 small packets on the data pipe
    // before normal report delivery starts. Index of the next blob to emit.
    UCHAR       InitStage;

    // Flipped after the very first control-pipe IN is satisfied with the
    // capabilities blob. Required to make XInputGetCapabilities work.
    BOOLEAN     ReportedCapabilities;

    // Last rumble bytes captured from xusb22 OUT transfers (kept for debug;
    // Steam Deck has no FF, so we don't forward them anywhere).
    UCHAR       Rumble[8];

    // LED slot index assigned by xusb22 (0..3 = controller 1..4).
    CHAR        LedNumber;
} SDBUS_PDO_CONTEXT, *PSDBUS_PDO_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SDBUS_PDO_CONTEXT, SdBusPdoGetContext)

// =============================================================================
//  PROTOTYPES
// =============================================================================

EXTERN_C_START

// BusDriver.cpp
DRIVER_INITIALIZE          DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD  SdBus_EvtDeviceAdd;

// BusFdo.cpp
NTSTATUS SdBusFdo_Create(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ USHORT          DefaultVendorId,
    _In_ USHORT          DefaultProductId);
NTSTATUS SdBusFdo_PlugInChild(_In_ WDFDEVICE Fdo);

EVT_WDF_DEVICE_PREPARE_HARDWARE     SdBusFdo_EvtPrepareHardware;
EVT_WDF_CHILD_LIST_CREATE_DEVICE    SdBusFdo_EvtChildListCreatePdo;
EVT_WDF_CHILD_LIST_IDENTIFICATION_DESCRIPTION_COMPARE
    SdBusFdo_EvtChildListIdentificationDescriptionCompare;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL  SdBusFdo_EvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL
    SdBusFdo_EvtIoInternalDeviceControl;

// BusPdo.cpp
NTSTATUS SdBusPdo_Create(
    _In_ WDFCHILDLIST                            ChildList,
    _In_ PSDBUS_PDO_IDENTIFICATION_DESCRIPTION   Desc,
    _In_ PWDFDEVICE_INIT                         DeviceInit);

NTSTATUS SdBusPdo_SubmitReport(
    _In_ WDFDEVICE              Fdo,
    _In_ PSDBUS_XUSB_REPORT     Report);

EVT_WDF_DEVICE_PREPARE_HARDWARE        SdBusPdo_EvtPrepareHardware;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL
    SdBusPdo_EvtIoInternalDeviceControl;

EXTERN_C_END
