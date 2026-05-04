//
// BusDriver.cpp - DriverEntry + EvtDeviceAdd for SteamDeckBus.sys
//
// Forked from ViGEmBus (BSD 3-Clause). Stripped to a single-target XUSB bus
// with no usermode plug/unplug API and no DS4. See BusDriver.h for topology.
//
// initguid.h MUST come before any header that wraps the GUIDs we need with
// DEFINE_GUID, so the macro emits storage instead of forward declarations.
// This TU materialises GUID_BUS_TYPE_USB, USB_BUS_INTERFACE_USBDI_GUID, and
// GUID_DEVINTERFACE_USB_DEVICE; thanks to __declspec(selectany) the linker
// dedupes if any other TU happens to define them too.
//
// Order matters: usbbusif.h depends on PURB / PUSBD_VERSION_INFORMATION
// from usb.h + usbdlib.h, and usbioctl.h pulls usbiodef.h transitively, so
// initguid.h has to precede usbioctl.h for the GUIDs in usbiodef.h to land.
#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>

#include <initguid.h>
#include <usbioctl.h>
#include <wdmguid.h>
#include <usbbusif.h>

#include "BusDriver.h"

extern "C" DRIVER_INITIALIZE DriverEntry;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, SdBus_EvtDeviceAdd)
#endif

// ---------------------------------------------------------------------------
// Read VendorId/ProductId overrides from the service Parameters key.
// HKLM\SYSTEM\CCS\Services\SteamDeckBus\Parameters\{VendorId,ProductId}
// Falls back to the Microsoft Xbox 360 wired pair if either key is missing.
// ---------------------------------------------------------------------------
static VOID
SdBus_LoadIdsFromRegistry(
    _In_  WDFDRIVER  Driver,
    _Out_ USHORT*    Vid,
    _Out_ USHORT*    Pid)
{
    WDFKEY paramsKey = nullptr;
    DECLARE_CONST_UNICODE_STRING(vidName, L"VendorId");
    DECLARE_CONST_UNICODE_STRING(pidName, L"ProductId");

    *Vid = SDBUS_DEFAULT_VID;
    *Pid = SDBUS_DEFAULT_PID;

    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(
        Driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &paramsKey)))
    {
        return;
    }

    ULONG value = 0;
    if (NT_SUCCESS(WdfRegistryQueryULong(paramsKey, &vidName, &value)) &&
        value != 0 && value <= 0xFFFF)
    {
        *Vid = (USHORT)value;
    }
    if (NT_SUCCESS(WdfRegistryQueryULong(paramsKey, &pidName, &value)) &&
        value != 0 && value <= 0xFFFF)
    {
        *Pid = (USHORT)value;
    }

    WdfRegistryClose(paramsKey);
}

// ---------------------------------------------------------------------------
// DriverEntry
// ---------------------------------------------------------------------------
extern "C" NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckBus: DriverEntry\n"));

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    WDF_DRIVER_CONFIG_INIT(&config, SdBus_EvtDeviceAdd);

    NTSTATUS status = WdfDriverCreate(DriverObject, RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckBus: WdfDriverCreate 0x%X\n", status));
    }

    return status;
}

// ---------------------------------------------------------------------------
// EvtDeviceAdd - one bus FDO; root enumerator delivers DeviceInit.
// ---------------------------------------------------------------------------
NTSTATUS
SdBus_EvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    USHORT vid, pid;

    PAGED_CODE();

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckBus: EvtDeviceAdd\n"));

    SdBus_LoadIdsFromRegistry(Driver, &vid, &pid);
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckBus: child VID/PID = %04X/%04X\n", vid, pid));

    return SdBusFdo_Create(DeviceInit, vid, pid);
}
