//
// Device.c  –  PnP / Power lifecycle
//
#include "Driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, SteamDeckDevice_Create)
#pragma alloc_text(PAGE, SteamDeckDevice_EvtPrepareHardware)
#pragma alloc_text(PAGE, SteamDeckDevice_EvtReleaseHardware)
#endif

NTSTATUS
SteamDeckDevice_Create(
    _In_     PWDFDEVICE_INIT   DeviceInit,
    _Outptr_opt_ WDFDEVICE    *Device)
{
    NTSTATUS                     status;
    WDF_OBJECT_ATTRIBUTES        attrs;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDFDEVICE                    device;
    PDEVICE_CONTEXT              ctx;

    PAGED_CODE();

    //
    // We are the FDO for USB\VID_28DE&PID_1205&MI_02. No HID stack above us:
    // games are reached via the SteamDeckBus virtual XUSB controller, not via
    // hidclass. Hence no mshidkmdf, no HidRegisterMinidriver, no filter mode.
    //
    //   SteamDeckHID.sys (FDO, us)  <- this driver
    //   usbccgp PDO (MI_02)         <- USB stack below
    //

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = SteamDeckDevice_EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = SteamDeckDevice_EvtReleaseHardware;
    pnp.EvtDeviceD0Entry         = SteamDeckDevice_EvtD0Entry;
    pnp.EvtDeviceD0Exit          = SteamDeckDevice_EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: WdfDeviceCreate failed 0x%X\n", status));
        return status;
    }

    ctx = DeviceGetContext(device);
    RtlZeroMemory(ctx, sizeof(DEVICE_CONTEXT));
    ctx->WdfDevice = device;
    KeInitializeSpinLock(&ctx->ReportLock);
    KeInitializeSpinLock(&ctx->XusbLock);

    if (Device) *Device = device;
    return STATUS_SUCCESS;
}

NTSTATUS
SteamDeckDevice_EvtPrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    NTSTATUS status;
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckHID: PrepareHardware\n"));

    status = SteamDeckUsb_Init(Device);
    if (!NT_SUCCESS(status)) return status;

    // Periodic Lizard-disable: DISPATCH-level timer + PASSIVE-level work
    // item that issues the synchronous USB control transfer. The previous
    // PASSIVE-level timer pattern was rejected by KMDF (parent device's
    // ExecutionLevel is DISPATCH) and surfaced as Code 10 from PnP start.
    status = SteamDeckUsb_CreateLizardTimer(Device);
    if (!NT_SUCCESS(status)) return status;

    // One-shot initial Lizard disable so the device exits Lizard immediately,
    // before the first 250 ms timer tick. Non-fatal if it fails.
    (VOID)SteamDeckUsb_DisableLizardMode(Device);

    // Resolve \Device\SteamDeckBus once at PASSIVE_LEVEL. ReadComplete runs
    // at DISPATCH where IoGetDeviceObjectPointer is illegal, so this is the
    // only place we can do the lookup.
    SteamDeckXInput_Resolve(Device);

    return STATUS_SUCCESS;
}

NTSTATUS
SteamDeckDevice_EvtReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckHID: ReleaseHardware\n"));

    // Drop our reference on \Device\SteamDeckBus, if held.
    SteamDeckXInput_Cleanup(Device);

    // Reader is already stopped by EvtD0Exit (called before ReleaseHardware
    // on every power-down path). Calling WdfIoTargetStop with
    // WdfIoTargetCancelSentIo here is redundant and risks deadlocking a
    // completion DPC mid-cancellation -> DPC_WATCHDOG_VIOLATION (0x133).
    return STATUS_SUCCESS;
}

NTSTATUS
SteamDeckDevice_EvtD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    NTSTATUS status;
    UNREFERENCED_PARAMETER(PreviousState);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckHID: D0Entry (prev=%d)\n", PreviousState));

    // Periodic Lizard-disable timer DISABLED for now while we isolate the
    // post-install slowdown. The one-shot DisableLizardMode in
    // EvtPrepareHardware still runs once. To re-enable, uncomment:
    SteamDeckUsb_StartLizardTimer(Device);

    status = SteamDeckUsb_StartReader(Device);
    if (!NT_SUCCESS(status))
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: StartReader failed 0x%X\n", status));

    return status;
}

NTSTATUS
SteamDeckDevice_EvtD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckHID: D0Exit (target=%d)\n", TargetState));

    // Stop the timer FIRST (Wait=TRUE inside) so no Lizard-disable USB
    // transfer is in flight while we tear the reader down.
    SteamDeckUsb_StopLizardTimer(Device);
    SteamDeckUsb_StopReader(Device);
    return STATUS_SUCCESS;
}
