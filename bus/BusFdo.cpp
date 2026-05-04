//
// BusFdo.cpp - root-enumerated bus FDO for SteamDeckBus.sys
//
// Responsibilities:
//   - Create a NAMED PnP device (\Device\SteamDeckBus) so SteamDeckHID can
//     find us with IoGetDeviceObjectPointer.
//   - Configure a single-element default child list. The PDO is auto-spawned
//     in PrepareHardware - no usermode plug/unplug API.
//   - Receive IOCTL_SDBUS_SUBMIT_XUSB on the default queue and fan it out
//     to the live child PDO.
//
#include "BusDriver.h"
// GUID_BUS_TYPE_USB is materialised in BusDriver.cpp; here we just reference
// the extern declaration, which wdmguid.h provides without DEFINE_GUID.
#include <wdmguid.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, SdBusFdo_Create)
#pragma alloc_text(PAGE, SdBusFdo_PlugInChild)
#pragma alloc_text(PAGE, SdBusFdo_EvtPrepareHardware)
#pragma alloc_text(PAGE, SdBusFdo_EvtChildListCreatePdo)
#endif

// ---------------------------------------------------------------------------
// SdBusFdo_Create
// ---------------------------------------------------------------------------
NTSTATUS
SdBusFdo_Create(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ USHORT          DefaultVendorId,
    _In_ USHORT          DefaultProductId)
{
    NTSTATUS                       status;
    WDF_OBJECT_ATTRIBUTES          attrs;
    WDF_PNPPOWER_EVENT_CALLBACKS   pnp;
    WDF_CHILD_LIST_CONFIG          childCfg;
    WDF_IO_QUEUE_CONFIG            queueCfg;
    PNP_BUS_INFORMATION            busInfo;
    WDFDEVICE                      device;
    PSDBUS_FDO_CONTEXT             ctx;
    DECLARE_CONST_UNICODE_STRING(deviceName, SDBUS_NT_DEVICE_NAME_W);

    PAGED_CODE();

    // Bus extender; named so the HID driver can open us by path.
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    status = WdfDeviceInitAssignName(DeviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckBus: AssignName 0x%X\n", status));
        return status;
    }

    // SDDL: System full, Admin full, no other users. The HID driver runs in
    // kernel mode so DACL doesn't gate it -- this just stops random usermode
    // processes from poking the submit IOCTL.
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckBus: AssignSDDL 0x%X\n", status));
        return status;
    }

    // Single child PDO; identification description size is fixed.
    WDF_CHILD_LIST_CONFIG_INIT(
        &childCfg,
        sizeof(SDBUS_PDO_IDENTIFICATION_DESCRIPTION),
        SdBusFdo_EvtChildListCreatePdo);
    childCfg.EvtChildListIdentificationDescriptionCompare =
        SdBusFdo_EvtChildListIdentificationDescriptionCompare;
    WdfFdoInitSetDefaultChildListConfig(DeviceInit, &childCfg, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = SdBusFdo_EvtPrepareHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, SDBUS_FDO_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckBus: WdfDeviceCreate 0x%X\n", status));
        return status;
    }

    ctx = SdBusFdoGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Device           = device;
    ctx->DefaultVendorId  = DefaultVendorId;
    ctx->DefaultProductId = DefaultProductId;
    ctx->ChildPlugged     = FALSE;

    // Tell the PnP manager our children belong to a USB-class bus. Without
    // this, child PDOs come up with the wrong BusTypeGuid and xusb22 never
    // binds. ViGEm uses GUID_BUS_TYPE_USB; we mirror it.
    busInfo.BusTypeGuid   = GUID_BUS_TYPE_USB;
    busInfo.LegacyBusType = PNPBus;
    busInfo.BusNumber     = 0;
    WdfDeviceSetBusInformationForChildren(device, &busInfo);

    // Default queue handles SUBMIT IOCTL from the HID driver. Both
    // EvtIoDeviceControl (METHOD_BUFFERED via DeviceIoControlFile) and
    // EvtIoInternalDeviceControl (kernel-mode IoCallDriver with internal
    // codes) get routed to the same handler; HID uses the latter.
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueCfg, WdfIoQueueDispatchParallel);
    queueCfg.EvtIoDeviceControl         = SdBusFdo_EvtIoDeviceControl;
    queueCfg.EvtIoInternalDeviceControl = SdBusFdo_EvtIoInternalDeviceControl;
    status = WdfIoQueueCreate(device, &queueCfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckBus: WdfIoQueueCreate 0x%X\n", status));
        return status;
    }

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// PrepareHardware: spawn the single XUSB child PDO.
// ---------------------------------------------------------------------------
NTSTATUS
SdBusFdo_EvtPrepareHardware(
    _In_ WDFDEVICE     Device,
    _In_ WDFCMRESLIST  ResourcesRaw,
    _In_ WDFCMRESLIST  ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckBus: PrepareHardware\n"));

    return SdBusFdo_PlugInChild(Device);
}

// ---------------------------------------------------------------------------
// SdBusFdo_PlugInChild - add the fixed XUSB PDO description to our child list.
// ---------------------------------------------------------------------------
NTSTATUS
SdBusFdo_PlugInChild(_In_ WDFDEVICE Fdo)
{
    SDBUS_PDO_IDENTIFICATION_DESCRIPTION desc;
    PSDBUS_FDO_CONTEXT                   ctx = SdBusFdoGetContext(Fdo);

    PAGED_CODE();

    if (ctx->ChildPlugged)
        return STATUS_SUCCESS;

    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&desc.Header, sizeof(desc));
    desc.SerialNo  = SDBUS_FIXED_SERIAL;
    desc.VendorId  = ctx->DefaultVendorId;
    desc.ProductId = ctx->DefaultProductId;

    NTSTATUS status = WdfChildListAddOrUpdateChildDescriptionAsPresent(
        WdfFdoGetDefaultChildList(Fdo),
        &desc.Header,
        nullptr);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckBus: AddOrUpdateChildDescriptionAsPresent 0x%X\n", status));
        return status;
    }

    ctx->ChildPlugged = TRUE;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// EvtChildListIdentificationDescriptionCompare
// We only ever have one child (serial == SDBUS_FIXED_SERIAL) so the compare
// is trivial.
// ---------------------------------------------------------------------------
BOOLEAN
SdBusFdo_EvtChildListIdentificationDescriptionCompare(
    _In_ WDFCHILDLIST DeviceList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER A,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER B)
{
    UNREFERENCED_PARAMETER(DeviceList);
    auto a = CONTAINING_RECORD(A, SDBUS_PDO_IDENTIFICATION_DESCRIPTION, Header);
    auto b = CONTAINING_RECORD(B, SDBUS_PDO_IDENTIFICATION_DESCRIPTION, Header);
    return (a->SerialNo == b->SerialNo) ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
// EvtChildListCreatePdo - delegate to BusPdo.cpp
// ---------------------------------------------------------------------------
NTSTATUS
SdBusFdo_EvtChildListCreatePdo(
    _In_ WDFCHILDLIST                                ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER  IdHeader,
    _In_ PWDFDEVICE_INIT                             DeviceInit)
{
    auto desc = CONTAINING_RECORD(IdHeader, SDBUS_PDO_IDENTIFICATION_DESCRIPTION, Header);
    return SdBusPdo_Create(ChildList, desc, DeviceInit);
}

// ---------------------------------------------------------------------------
// Locate the live PDO context. Single-target bus, so we just take the first
// present child. NULL if PnP hasn't created the PDO yet (PrepareHardware
// race during early start) or it was removed.
// ---------------------------------------------------------------------------
static PSDBUS_PDO_CONTEXT
SdBusFdo_GetChild(_In_ WDFDEVICE Fdo)
{
    WDFCHILDLIST                         list = WdfFdoGetDefaultChildList(Fdo);
    WDF_CHILD_LIST_ITERATOR              it;
    WDF_CHILD_RETRIEVE_INFO              info;
    SDBUS_PDO_IDENTIFICATION_DESCRIPTION desc;
    WDFDEVICE                            child;
    PSDBUS_PDO_CONTEXT                   result = nullptr;

    WDF_CHILD_LIST_ITERATOR_INIT(&it, WdfRetrievePresentChildren);
    WdfChildListBeginIteration(list, &it);

    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&desc.Header, sizeof(desc));
    WDF_CHILD_RETRIEVE_INFO_INIT(&info, &desc.Header);

    if (NT_SUCCESS(WdfChildListRetrieveNextDevice(list, &it, &child, &info)) &&
        info.Status == WdfChildListRetrieveDeviceSuccess)
    {
        result = SdBusPdoGetContext(child);
    }

    WdfChildListEndIteration(list, &it);
    return result;
}

// ---------------------------------------------------------------------------
// Submit IOCTL handler shared by both Device and InternalDeviceControl paths.
// ---------------------------------------------------------------------------
static VOID
SdBusFdo_HandleSubmit(
    _In_ WDFDEVICE Fdo,
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode)
{
    if (IoControlCode != IOCTL_SDBUS_SUBMIT_XUSB) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    PSDBUS_XUSB_REPORT report = nullptr;
    size_t              len   = 0;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(
        Request, sizeof(SDBUS_XUSB_REPORT),
        reinterpret_cast<PVOID*>(&report), &len);
    if (!NT_SUCCESS(status) || len < sizeof(SDBUS_XUSB_REPORT)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    status = SdBusPdo_SubmitReport(Fdo, report);
    WdfRequestComplete(Request, status);
}

VOID
SdBusFdo_EvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    SdBusFdo_HandleSubmit(WdfIoQueueGetDevice(Queue), Request, IoControlCode);
}

VOID
SdBusFdo_EvtIoInternalDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    SdBusFdo_HandleSubmit(WdfIoQueueGetDevice(Queue), Request, IoControlCode);
}

// Expose for BusPdo.cpp to grab the live child when SubmitReport runs.
EXTERN_C PSDBUS_PDO_CONTEXT
SdBusFdo_FindChildContext(_In_ WDFDEVICE Fdo)
{
    return SdBusFdo_GetChild(Fdo);
}
