//
// Driver.c  -  DriverEntry + EvtDeviceAdd
//
// Pattern: vhidmini2 (KMDF) adapted for USB hardware.
//
//   hidclass.sys       (upper filter, loaded by mshidkmdf)
//   mshidkmdf.sys      (FDO - replaces our old HidRegisterMinidriver call)
//   SteamDeckHID.sys   (LOWER FILTER - us; does USB I/O + IOCTL_HID_*)
//   usbccgp PDO        (USB\VID_28DE&PID_1205&MI_02)
//
// We do NOT call HidRegisterMinidriver. The INF stacks mshidkmdf above us
// via "Include=MsHidKmdf.inf" + "Needs=MsHidKmdf.NT*", and "AddFilter ...
// FilterPosition=Lower" puts us below mshidkmdf. mshidkmdf forwards
// IOCTL_HID_* down to us via IRP_MJ_INTERNAL_DEVICE_CONTROL, which our
// EvtIoInternalDeviceControl handler picks up.
//
#include "Driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, SteamDeckHID_EvtDeviceAdd)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS          status;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckHID: DriverEntry\n"));

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    WDF_DRIVER_CONFIG_INIT(&config, SteamDeckHID_EvtDeviceAdd);

    status = WdfDriverCreate(DriverObject, RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(status))
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: WdfDriverCreate failed 0x%X\n", status));

    return status;
}

NTSTATUS
SteamDeckHID_EvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckHID: EvtDeviceAdd\n"));

    status = SteamDeckDevice_Create(DeviceInit, NULL);
    if (!NT_SUCCESS(status))
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: SteamDeckDevice_Create failed 0x%X\n", status));

    return status;
}
