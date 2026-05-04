//
// BusPdo.cpp - virtual XUSB PDO for SteamDeckBus.sys
//
// Ported (with simplifications) from ViGEmBus' EmulationTargetXUSB. The
// reverse-engineered data - configuration descriptor, "boot sequence" blobs,
// the trailing 0x31 0x3F 0xCF 0xDC magic - is preserved verbatim because
// xusb22.sys keys off these exact byte patterns.
//
// Compared to ViGEm:
//   - No DMF, no BufferQueue (we don't carry rumble back to anyone).
//   - No notification queue / inverted call API.
//   - No usermode owner check; submission comes from a kernel driver.
//   - Single-target only; the PDO context is a POD struct, no C++ classes.
//
// References (line numbers in ViGEmBus-master):
//   sys/XusbPdo.cpp         - descriptor blob + URB hijack + init stages
//   sys/EmulationTargetPDO.cpp - Pdo create + URB dispatch
//
#include "BusDriver.h"
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>
// USB_BUS_INTERFACE_USBDI_GUID and GUID_DEVINTERFACE_USB_DEVICE are
// materialised in BusDriver.cpp.

extern "C" PSDBUS_PDO_CONTEXT SdBusFdo_FindChildContext(_In_ WDFDEVICE Fdo);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, SdBusPdo_Create)
#endif

// =============================================================================
//  XUSB descriptor blob
//  Source: ViGEmBus-master/sys/XusbPdo.cpp lines 297-432
// =============================================================================
#define SDBUS_XUSB_DESCRIPTOR_SIZE  0x99

static const UCHAR SdBus_XusbDescriptor[SDBUS_XUSB_DESCRIPTOR_SIZE] =
{
    0x09,        // bLength
    0x02,        // bDescriptorType (Configuration)
    0x99, 0x00,  // wTotalLength 153
    0x04,        // bNumInterfaces
    0x01,        // bConfigurationValue
    0x00,        // iConfiguration
    0xA0,        // bmAttributes (Remote Wakeup)
    0xFA,        // bMaxPower 500mA

    0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x5D, 0x01, 0x00,  // Interface 0
    0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81,
    0x14, 0x00, 0x00, 0x00, 0x00,
    0x13, 0x01, 0x08, 0x00, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,  // EP 0x81 IN  Interrupt
    0x07, 0x05, 0x01, 0x03, 0x20, 0x00, 0x08,  // EP 0x01 OUT Interrupt

    0x09, 0x04, 0x01, 0x00, 0x04, 0xFF, 0x5D, 0x03, 0x00,  // Interface 1
    0x1B, 0x21, 0x00, 0x01, 0x01, 0x01, 0x82, 0x40, 0x01,
    0x02, 0x20, 0x16, 0x83, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x16, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x82, 0x03, 0x20, 0x00, 0x02,  // EP 0x82 IN
    0x07, 0x05, 0x02, 0x03, 0x20, 0x00, 0x04,  // EP 0x02 OUT
    0x07, 0x05, 0x83, 0x03, 0x20, 0x00, 0x40,  // EP 0x83 IN  Control
    0x07, 0x05, 0x03, 0x03, 0x20, 0x00, 0x10,  // EP 0x03 OUT

    0x09, 0x04, 0x02, 0x00, 0x01, 0xFF, 0x5D, 0x02, 0x00,  // Interface 2
    0x09, 0x21, 0x00, 0x01, 0x01, 0x22, 0x84, 0x07, 0x00,
    0x07, 0x05, 0x84, 0x03, 0x20, 0x00, 0x10,  // EP 0x84 IN

    0x09, 0x04, 0x03, 0x00, 0x00, 0xFF, 0xFD, 0x13, 0x04,  // Interface 3
    0x06, 0x41, 0x00, 0x01, 0x01, 0x03,
};
C_ASSERT(sizeof(SdBus_XusbDescriptor) == SDBUS_XUSB_DESCRIPTOR_SIZE);

// =============================================================================
//  Boot-sequence blobs
//  Source: ViGEmBus-master/sys/XusbPdo.cpp lines 250-269
// =============================================================================
#define SDBUS_INIT_STAGE_SIZE  3
#define SDBUS_PACKET_OFFSET    12     // blob 4 = full XUSB packet (20 bytes)

static const UCHAR SdBus_InitBlobs[] =
{
    // 0
    0x01, 0x03, 0x0E,
    // 1
    0x02, 0x03, 0x00,
    // 2
    0x03, 0x03, 0x03,
    // 3
    0x08, 0x03, 0x00,
    // 4 - 20-byte XUSB-packet-shaped boot record
    0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0xE4, 0xF2,
    0xB3, 0xF8, 0x49, 0xF3, 0xB0, 0xFC, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    // 5
    0x01, 0x03, 0x03,
    // 6 - capabilities answer for the control pipe
    0x05, 0x03, 0x00,
    // 7 - "Xenon magic" returned to a specific control transfer
    0x31, 0x3F, 0xCF, 0xDC,
};

// Offsets into SdBus_InitBlobs[]
enum {
    SDBUS_BLOB_BOOT0  = 0,
    SDBUS_BLOB_BOOT1  = 3,
    SDBUS_BLOB_BOOT2  = 6,
    SDBUS_BLOB_BOOT3  = 9,
    SDBUS_BLOB_PACKET = SDBUS_PACKET_OFFSET,
    SDBUS_BLOB_BOOT5  = 32,
    SDBUS_BLOB_CAPS   = 35,
    SDBUS_BLOB_XENON  = 38,
};

// Pipe handles xusb22 sees in the SELECT_CONFIGURATION response. Constants;
// we only need to recognise them on subsequent BULK_OR_INTERRUPT_TRANSFERs.
#define SDBUS_PIPE_DATA_IN_HANDLE   ((USBD_PIPE_HANDLE)(ULONG_PTR)0xFFFF0081)
#define SDBUS_PIPE_CTL_IN_HANDLE    ((USBD_PIPE_HANDLE)(ULONG_PTR)0xFFFF0083)

// =============================================================================
//  USB BUS INTERFACE callbacks
//  xusb22.sys queries USB_BUS_INTERFACE_USBDI_GUID on the PDO before issuing
//  any URBs. We must answer with a few stub bus-info functions so it
//  considers the device a high-speed USB bus.
//  Source: ViGEmBus-master/sys/EmulationTargetPDO.cpp lines 593-651
// =============================================================================
static BOOLEAN USB_BUSIFFN
SdBus_UsbIfaceIsHighSpeed(_In_ PVOID BusContext)
{
    UNREFERENCED_PARAMETER(BusContext);
    return TRUE;
}

static NTSTATUS USB_BUSIFFN
SdBus_UsbIfaceQueryBusInformation(
    _In_ PVOID, _In_ ULONG, _Inout_ PVOID, _Inout_ PULONG, _Out_ PULONG)
{
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS USB_BUSIFFN
SdBus_UsbIfaceSubmitIsoOutUrb(_In_ PVOID, _In_ PURB)
{
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS USB_BUSIFFN
SdBus_UsbIfaceQueryBusTime(_In_ PVOID, _Inout_ PULONG)
{
    return STATUS_UNSUCCESSFUL;
}

static VOID USB_BUSIFFN
SdBus_UsbIfaceGetUSBDIVersion(
    _In_ PVOID,
    _Inout_opt_ PUSBD_VERSION_INFORMATION VersionInformation,
    _Inout_opt_ PULONG HcdCapabilities)
{
    if (VersionInformation) {
        VersionInformation->USBDI_Version          = 0x500;
        VersionInformation->Supported_USB_Version  = 0x200;
    }
    if (HcdCapabilities)
        *HcdCapabilities = 0;
}

// =============================================================================
//  USB descriptor responses
// =============================================================================
static NTSTATUS
SdBus_UsbGetDeviceDescriptor(
    _In_  PSDBUS_PDO_CONTEXT      Pdo,
    _Out_ PUSB_DEVICE_DESCRIPTOR  D)
{
    D->bLength            = sizeof(USB_DEVICE_DESCRIPTOR);
    D->bDescriptorType    = USB_DEVICE_DESCRIPTOR_TYPE;
    D->bcdUSB             = 0x0200;
    D->bDeviceClass       = 0xFF;
    D->bDeviceSubClass    = 0xFF;
    D->bDeviceProtocol    = 0xFF;
    D->bMaxPacketSize0    = 0x08;
    D->idVendor           = Pdo->VendorId;
    D->idProduct          = Pdo->ProductId;
    D->bcdDevice          = 0x0114;
    D->iManufacturer      = 0x01;
    D->iProduct           = 0x02;
    D->iSerialNumber      = 0x03;
    D->bNumConfigurations = 0x01;
    return STATUS_SUCCESS;
}

static VOID
SdBus_GetConfigurationDescriptor(
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length)
{
    if (Length > sizeof(SdBus_XusbDescriptor))
        Length = sizeof(SdBus_XusbDescriptor);
    RtlCopyMemory(Buffer, SdBus_XusbDescriptor, Length);
}

static NTSTATUS
SdBus_UsbGetConfigDescriptorURB(_In_ PURB Urb)
{
    PUCHAR buffer = static_cast<PUCHAR>(Urb->UrbControlDescriptorRequest.TransferBuffer);
    ULONG  length = Urb->UrbControlDescriptorRequest.TransferBufferLength;

    // First request only asks for the 9-byte header to learn wTotalLength.
    if (length == sizeof(USB_CONFIGURATION_DESCRIPTOR)) {
        SdBus_GetConfigurationDescriptor(buffer, length);
        return STATUS_SUCCESS;
    }
    if (length >= SDBUS_XUSB_DESCRIPTOR_SIZE) {
        SdBus_GetConfigurationDescriptor(buffer, SDBUS_XUSB_DESCRIPTOR_SIZE);
        return STATUS_SUCCESS;
    }
    return STATUS_UNSUCCESSFUL;
}

// =============================================================================
//  SELECT_CONFIGURATION / SELECT_INTERFACE
//  Source: ViGEmBus-master/sys/XusbPdo.cpp lines 457-692
// =============================================================================
static VOID
SdBus_FillPipe(
    _Out_ PUSBD_PIPE_INFORMATION p,
    UCHAR endpoint, UCHAR interval, USHORT handleLow)
{
    p->MaximumTransferSize = 0x00400000;
    p->MaximumPacketSize   = 0x20;
    p->EndpointAddress     = endpoint;
    p->Interval            = interval;
    p->PipeType            = (USBD_PIPE_TYPE)0x03;   // Interrupt
    p->PipeHandle          = (USBD_PIPE_HANDLE)(ULONG_PTR)(0xFFFF0000 | handleLow);
    p->PipeFlags           = 0x00;
}

static NTSTATUS
SdBus_UsbSelectConfiguration(_In_ PURB Urb)
{
    if (Urb->UrbHeader.Length == sizeof(struct _URB_SELECT_CONFIGURATION))
        return STATUS_SUCCESS;  // null configuration descriptor

    PUSBD_INTERFACE_INFORMATION pInfo = &Urb->UrbSelectConfiguration.Interface;

    // Interface 0 (data pipes 0x81 IN, 0x01 OUT)
    pInfo->Class           = 0xFF;
    pInfo->SubClass        = 0x5D;
    pInfo->Protocol        = 0x01;
    pInfo->InterfaceHandle = (USBD_INTERFACE_HANDLE)(ULONG_PTR)0xFFFF0000;
    SdBus_FillPipe(&pInfo->Pipes[0], 0x81, 0x04, 0x0081);
    SdBus_FillPipe(&pInfo->Pipes[1], 0x01, 0x08, 0x0001);

    pInfo = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)pInfo + pInfo->Length);

    // Interface 1 (control + audio pipes)
    pInfo->Class           = 0xFF;
    pInfo->SubClass        = 0x5D;
    pInfo->Protocol        = 0x03;
    pInfo->InterfaceHandle = (USBD_INTERFACE_HANDLE)(ULONG_PTR)0xFFFF0000;
    SdBus_FillPipe(&pInfo->Pipes[0], 0x82, 0x04, 0x0082);
    SdBus_FillPipe(&pInfo->Pipes[1], 0x02, 0x08, 0x0002);
    SdBus_FillPipe(&pInfo->Pipes[2], 0x83, 0x08, 0x0083);
    SdBus_FillPipe(&pInfo->Pipes[3], 0x03, 0x08, 0x0003);

    pInfo = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)pInfo + pInfo->Length);

    // Interface 2 (single IN pipe)
    pInfo->Class           = 0xFF;
    pInfo->SubClass        = 0x5D;
    pInfo->Protocol        = 0x02;
    pInfo->InterfaceHandle = (USBD_INTERFACE_HANDLE)(ULONG_PTR)0xFFFF0000;
    SdBus_FillPipe(&pInfo->Pipes[0], 0x84, 0x04, 0x0084);

    pInfo = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)pInfo + pInfo->Length);

    // Interface 3 (vendor; no pipes)
    pInfo->Class           = 0xFF;
    pInfo->SubClass        = 0xFD;
    pInfo->Protocol        = 0x13;
    pInfo->InterfaceHandle = (USBD_INTERFACE_HANDLE)(ULONG_PTR)0xFFFF0000;

    return STATUS_SUCCESS;
}

static NTSTATUS
SdBus_UsbSelectInterface(_In_ PURB Urb)
{
    PUSBD_INTERFACE_INFORMATION pInfo = &Urb->UrbSelectInterface.Interface;

    if (pInfo->InterfaceNumber == 1) {
        pInfo->Class           = 0xFF;
        pInfo->SubClass        = 0x5D;
        pInfo->Protocol        = 0x03;
        pInfo->NumberOfPipes   = 4;
        pInfo->InterfaceHandle = (USBD_INTERFACE_HANDLE)(ULONG_PTR)0xFFFF0000;
        SdBus_FillPipe(&pInfo->Pipes[0], 0x82, 0x04, 0x0082);
        SdBus_FillPipe(&pInfo->Pipes[1], 0x02, 0x08, 0x0002);
        SdBus_FillPipe(&pInfo->Pipes[2], 0x83, 0x08, 0x0083);
        SdBus_FillPipe(&pInfo->Pipes[3], 0x03, 0x08, 0x0003);
        return STATUS_SUCCESS;
    }
    if (pInfo->InterfaceNumber == 2) {
        pInfo->Class           = 0xFF;
        pInfo->SubClass        = 0x5D;
        pInfo->Protocol        = 0x02;
        pInfo->NumberOfPipes   = 1;
        pInfo->InterfaceHandle = (USBD_INTERFACE_HANDLE)(ULONG_PTR)0xFFFF0000;
        SdBus_FillPipe(&pInfo->Pipes[0], 0x84, 0x04, 0x0084);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

// =============================================================================
//  BULK_OR_INTERRUPT_TRANSFER
//  Sources: ViGEmBus-master/sys/XusbPdo.cpp lines 701-931
//
//  The data pipe (0x81) is what xusb22 polls for input reports. We pend it
//  in PendingDataIn and complete it from SubmitReport. The control pipe
//  (0x83) is auxiliary and gets the capabilities blob exactly once.
// =============================================================================
static NTSTATUS
SdBus_UsbBulkOrInterruptIN(
    _In_ PSDBUS_PDO_CONTEXT                Pdo,
    _In_ struct _URB_BULK_OR_INTERRUPT_TRANSFER* Xfer,
    _In_ WDFREQUEST                        Request)
{
    PUCHAR buf  = static_cast<PUCHAR>(Xfer->TransferBuffer);

    if (Xfer->PipeHandle == SDBUS_PIPE_DATA_IN_HANDLE)
    {
        // Replay the boot sequence first, then queue real reports.
        switch (Pdo->InitStage)
        {
        case 0:
            Xfer->TransferBufferLength = SDBUS_INIT_STAGE_SIZE;
            RtlCopyMemory(buf, &SdBus_InitBlobs[SDBUS_BLOB_BOOT0], SDBUS_INIT_STAGE_SIZE);
            Pdo->InitStage++;
            return STATUS_SUCCESS;
        case 1:
            Xfer->TransferBufferLength = SDBUS_INIT_STAGE_SIZE;
            RtlCopyMemory(buf, &SdBus_InitBlobs[SDBUS_BLOB_BOOT1], SDBUS_INIT_STAGE_SIZE);
            Pdo->InitStage++;
            return STATUS_SUCCESS;
        case 2:
            Xfer->TransferBufferLength = SDBUS_INIT_STAGE_SIZE;
            RtlCopyMemory(buf, &SdBus_InitBlobs[SDBUS_BLOB_BOOT2], SDBUS_INIT_STAGE_SIZE);
            Pdo->InitStage++;
            return STATUS_SUCCESS;
        case 3:
            Xfer->TransferBufferLength = SDBUS_INIT_STAGE_SIZE;
            RtlCopyMemory(buf, &SdBus_InitBlobs[SDBUS_BLOB_BOOT3], SDBUS_INIT_STAGE_SIZE);
            Pdo->InitStage++;
            return STATUS_SUCCESS;
        case 4:
            Xfer->TransferBufferLength = sizeof(SDBUS_XUSB_PACKET);
            RtlCopyMemory(buf, &SdBus_InitBlobs[SDBUS_BLOB_PACKET], sizeof(SDBUS_XUSB_PACKET));
            Pdo->InitStage++;
            return STATUS_SUCCESS;
        case 5:
            Xfer->TransferBufferLength = SDBUS_INIT_STAGE_SIZE;
            RtlCopyMemory(buf, &SdBus_InitBlobs[SDBUS_BLOB_BOOT5], SDBUS_INIT_STAGE_SIZE);
            Pdo->InitStage++;
            return STATUS_SUCCESS;
        default:
            // Pend until SubmitReport completes us.
            NTSTATUS status = WdfRequestForwardToIoQueue(Request, Pdo->PendingDataIn);
            return NT_SUCCESS(status) ? STATUS_PENDING : status;
        }
    }

    if (Xfer->PipeHandle == SDBUS_PIPE_CTL_IN_HANDLE)
    {
        // Capabilities answer first - XInputGetCapabilities depends on it.
        if (!Pdo->ReportedCapabilities &&
            Xfer->TransferBufferLength >= SDBUS_INIT_STAGE_SIZE)
        {
            RtlCopyMemory(buf, &SdBus_InitBlobs[SDBUS_BLOB_CAPS], SDBUS_INIT_STAGE_SIZE);
            Xfer->TransferBufferLength = SDBUS_INIT_STAGE_SIZE;
            Pdo->ReportedCapabilities  = TRUE;
            return STATUS_SUCCESS;
        }
        // Subsequent control IN polls just sit in the queue indefinitely.
        NTSTATUS status = WdfRequestForwardToIoQueue(Request, Pdo->PendingCtlIn);
        return NT_SUCCESS(status) ? STATUS_PENDING : status;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
SdBus_UsbBulkOrInterruptOUT(
    _In_ PSDBUS_PDO_CONTEXT Pdo,
    _In_ struct _URB_BULK_OR_INTERRUPT_TRANSFER* Xfer)
{
    // LED slot assignment (3-byte command starting 0x01 0x03 0x02..0x05).
    if (Xfer->TransferBufferLength == 3) {
        PUCHAR p = static_cast<PUCHAR>(Xfer->TransferBuffer);
        if (p[0] == 0x01 && p[1] == 0x03 && p[2] >= 0x02 && p[2] <= 0x05)
            Pdo->LedNumber = (CHAR)(p[2] - 0x02);
    }
    // Rumble (8 bytes) - cached but Steam Deck has no FF.
    else if (Xfer->TransferBufferLength == 8) {
        RtlCopyMemory(Pdo->Rumble, Xfer->TransferBuffer, 8);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
SdBus_UsbBulkOrInterruptTransfer(
    _In_ PSDBUS_PDO_CONTEXT Pdo,
    _In_ struct _URB_BULK_OR_INTERRUPT_TRANSFER* Xfer,
    _In_ WDFREQUEST                              Request)
{
    if (Xfer->TransferFlags & USBD_TRANSFER_DIRECTION_IN)
        return SdBus_UsbBulkOrInterruptIN(Pdo, Xfer, Request);
    return SdBus_UsbBulkOrInterruptOUT(Pdo, Xfer);
}

// =============================================================================
//  CONTROL_TRANSFER
//  Source: ViGEmBus-master/sys/XusbPdo.cpp lines 933-974
//  xusb22 issues a vendor-specific control with bRequest=0x04 expecting the
//  4-byte "Xenon magic". 0x14 and 0x08 are explicitly stalled.
// =============================================================================
static NTSTATUS
SdBus_UsbControlTransfer(_In_ PURB Urb)
{
    UCHAR bRequest = Urb->UrbControlTransfer.SetupPacket[6];
    switch (bRequest)
    {
    case 0x04:
        RtlCopyMemory(Urb->UrbControlTransfer.TransferBuffer,
                      &SdBus_InitBlobs[SDBUS_BLOB_XENON], 0x04);
        return STATUS_SUCCESS;
    case 0x14:
    case 0x08:
        Urb->UrbControlTransfer.Hdr.Status = USBD_STATUS_STALL_PID;
        return STATUS_UNSUCCESSFUL;
    default:
        return STATUS_SUCCESS;
    }
}

// =============================================================================
//  Main URB dispatcher (PDO default queue)
//  Source: ViGEmBus-master/sys/EmulationTargetPDO.cpp lines 964-1213
// =============================================================================
VOID
SdBusPdo_EvtIoInternalDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PSDBUS_PDO_CONTEXT pdo  = SdBusPdoGetContext(WdfIoQueueGetDevice(Queue));
    PIRP               irp  = WdfRequestWdmGetIrp(Request);
    PIO_STACK_LOCATION sl   = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS           status = STATUS_INVALID_PARAMETER;

    switch (IoControlCode)
    {
    case IOCTL_INTERNAL_USB_SUBMIT_URB:
    {
        PURB urb = static_cast<PURB>(URB_FROM_IRP(irp));
        switch (urb->UrbHeader.Function)
        {
        case URB_FUNCTION_CONTROL_TRANSFER:
            status = SdBus_UsbControlTransfer(urb);
            break;

        case URB_FUNCTION_CONTROL_TRANSFER_EX:
            status = STATUS_UNSUCCESSFUL;
            break;

        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
            status = SdBus_UsbBulkOrInterruptTransfer(
                pdo, &urb->UrbBulkOrInterruptTransfer, Request);
            break;

        case URB_FUNCTION_SELECT_CONFIGURATION:
            status = SdBus_UsbSelectConfiguration(urb);
            break;

        case URB_FUNCTION_SELECT_INTERFACE:
            status = SdBus_UsbSelectInterface(urb);
            break;

        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
            switch (urb->UrbControlDescriptorRequest.DescriptorType)
            {
            case USB_DEVICE_DESCRIPTOR_TYPE:
                status = SdBus_UsbGetDeviceDescriptor(pdo,
                    static_cast<PUSB_DEVICE_DESCRIPTOR>(urb->UrbControlDescriptorRequest.TransferBuffer));
                break;
            case USB_CONFIGURATION_DESCRIPTOR_TYPE:
                status = SdBus_UsbGetConfigDescriptorURB(urb);
                break;
            case USB_STRING_DESCRIPTOR_TYPE:
                status = STATUS_NOT_IMPLEMENTED;
                break;
            default:
                break;
            }
            break;

        case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
            status = STATUS_SUCCESS;
            break;

        case URB_FUNCTION_ABORT_PIPE:
            // Higher driver shutting down - flush both pending queues.
            WdfIoQueuePurge(pdo->PendingDataIn, nullptr, nullptr);
            WdfIoQueuePurge(pdo->PendingCtlIn,  nullptr, nullptr);
            break;

        case URB_FUNCTION_CLASS_INTERFACE:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
            status = STATUS_NOT_IMPLEMENTED;
            break;

        default:
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                "SteamDeckBus: unhandled URB function 0x%X\n",
                urb->UrbHeader.Function));
            break;
        }
        break;
    }

    case IOCTL_INTERNAL_USB_GET_PORT_STATUS:
        *static_cast<ULONG*>(sl->Parameters.Others.Argument1) =
            USBD_PORT_ENABLED | USBD_PORT_CONNECTED;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_INTERNAL_USB_RESET_PORT:
    case IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION:
        status = STATUS_SUCCESS;
        break;

    default:
        break;
    }

    if (status != STATUS_PENDING)
        WdfRequestComplete(Request, status);
}

// =============================================================================
//  PrepareHardware: install the USB query interface so xusb22 will talk to us.
// =============================================================================
NTSTATUS
SdBusPdo_EvtPrepareHardware(
    _In_ WDFDEVICE     Device,
    _In_ WDFCMRESLIST  ResourcesRaw,
    _In_ WDFCMRESLIST  ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    USB_BUS_INTERFACE_USBDI_V1   iface;
    WDF_QUERY_INTERFACE_CONFIG   ifaceCfg;

    iface.Size                  = sizeof(iface);
    iface.Version               = USB_BUSIF_USBDI_VERSION_1;
    iface.BusContext            = static_cast<PVOID>(Device);
    iface.InterfaceReference    = WdfDeviceInterfaceReferenceNoOp;
    iface.InterfaceDereference  = WdfDeviceInterfaceDereferenceNoOp;
    iface.SubmitIsoOutUrb       = SdBus_UsbIfaceSubmitIsoOutUrb;
    iface.GetUSBDIVersion       = SdBus_UsbIfaceGetUSBDIVersion;
    iface.QueryBusTime          = SdBus_UsbIfaceQueryBusTime;
    iface.QueryBusInformation   = SdBus_UsbIfaceQueryBusInformation;
    iface.IsDeviceHighSpeed     = SdBus_UsbIfaceIsHighSpeed;

    WDF_QUERY_INTERFACE_CONFIG_INIT(
        &ifaceCfg,
        reinterpret_cast<PINTERFACE>(&iface),
        &USB_BUS_INTERFACE_USBDI_GUID,
        nullptr);

    NTSTATUS status = WdfDeviceAddQueryInterface(Device, &ifaceCfg);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckBus: AddQueryInterface 0x%X\n", status));
    }
    return status;
}

// =============================================================================
//  PDO creation
//  Source: ViGEmBus-master/sys/EmulationTargetPDO.cpp lines 48-372
// =============================================================================
NTSTATUS
SdBusPdo_Create(
    _In_ WDFCHILDLIST                            ChildList,
    _In_ PSDBUS_PDO_IDENTIFICATION_DESCRIPTION   Desc,
    _In_ PWDFDEVICE_INIT                         DeviceInit)
{
    NTSTATUS                       status;
    WDF_PNPPOWER_EVENT_CALLBACKS   pnp;
    WDF_OBJECT_ATTRIBUTES          attrs;
    WDF_IO_QUEUE_CONFIG            qcfg;
    WDF_DEVICE_PNP_CAPABILITIES    pnpCaps;
    WDF_DEVICE_POWER_CAPABILITIES  powCaps;
    WDFDEVICE                      pdo  = nullptr;
    PSDBUS_PDO_CONTEXT             ctx;
    DECLARE_UNICODE_STRING_SIZE(deviceId,        80);
    DECLARE_UNICODE_STRING_SIZE(instanceId,      32);
    DECLARE_CONST_UNICODE_STRING(deviceLocation, L"Steam Deck XInput Bus");
    DECLARE_CONST_UNICODE_STRING(deviceDesc,     L"Virtual Xbox 360 Controller");

    PAGED_CODE();

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);

    // Hardware ID drives which class driver attaches: USB\VID_xxxx&PID_yyyy
    // is what xusb22's INF claims.
    status = RtlUnicodeStringPrintf(&deviceId, L"USB\\VID_%04X&PID_%04X",
        Desc->VendorId, Desc->ProductId);
    if (!NT_SUCCESS(status)) return status;

    status = WdfPdoInitAssignDeviceID(DeviceInit, &deviceId);
    if (!NT_SUCCESS(status)) return status;
    status = WdfPdoInitAddHardwareID(DeviceInit, &deviceId);
    if (!NT_SUCCESS(status)) return status;

    // Compatible IDs - critical: USB\MS_COMP_XUSB10 is what causes xusb22.sys
    // to bind. Without it, Windows shows an unknown USB device.
    DECLARE_CONST_UNICODE_STRING(compatXusb,     L"USB\\MS_COMP_XUSB10");
    DECLARE_CONST_UNICODE_STRING(compatClassFF1, L"USB\\Class_FF&SubClass_5D&Prot_01");
    DECLARE_CONST_UNICODE_STRING(compatClassFF2, L"USB\\Class_FF&SubClass_5D");
    DECLARE_CONST_UNICODE_STRING(compatClassFF3, L"USB\\Class_FF");
    if (!NT_SUCCESS(status = WdfPdoInitAddCompatibleID(DeviceInit, &compatXusb)))    return status;
    if (!NT_SUCCESS(status = WdfPdoInitAddCompatibleID(DeviceInit, &compatClassFF1))) return status;
    if (!NT_SUCCESS(status = WdfPdoInitAddCompatibleID(DeviceInit, &compatClassFF2))) return status;
    if (!NT_SUCCESS(status = WdfPdoInitAddCompatibleID(DeviceInit, &compatClassFF3))) return status;

    status = RtlUnicodeStringPrintf(&instanceId, L"%02u", Desc->SerialNo);
    if (!NT_SUCCESS(status)) return status;
    status = WdfPdoInitAssignInstanceID(DeviceInit, &instanceId);
    if (!NT_SUCCESS(status)) return status;

    status = WdfPdoInitAddDeviceText(DeviceInit, &deviceDesc, &deviceLocation, 0x409);
    if (!NT_SUCCESS(status)) return status;
    WdfPdoInitSetDefaultLocale(DeviceInit, 0x409);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = SdBusPdo_EvtPrepareHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, SDBUS_PDO_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &pdo);
    if (!NT_SUCCESS(status)) return status;

    // Expose USB device interface so usermode HID/raw-input enumeration sees
    // our PDO as a USB device (matches ViGEm's behaviour).
    status = WdfDeviceCreateDeviceInterface(pdo,
        const_cast<LPGUID>(&GUID_DEVINTERFACE_USB_DEVICE), nullptr);
    if (!NT_SUCCESS(status)) return status;

    ctx = SdBusPdoGetContext(pdo);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->PdoDevice = pdo;
    ctx->ParentFdo = WdfChildListGetDevice(ChildList);
    ctx->VendorId  = Desc->VendorId;
    ctx->ProductId = Desc->ProductId;
    ctx->SerialNo  = Desc->SerialNo;
    ctx->LedNumber = -1;
    KeInitializeSpinLock(&ctx->PacketLock);

    // Empty starting packet (size byte must be 0x14 even when idle).
    ctx->Packet.Id   = 0x00;
    ctx->Packet.Size = 0x14;

    // Default queue catches every URB IRP from xusb22.
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoInternalDeviceControl = SdBusPdo_EvtIoInternalDeviceControl;
    status = WdfIoQueueCreate(pdo, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;

    // Manual queue holding pending IN URBs from the data pipe.
    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(pdo, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->PendingDataIn);
    if (!NT_SUCCESS(status)) return status;

    // Manual queue for the auxiliary control pipe (mostly idle).
    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(pdo, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->PendingCtlIn);
    if (!NT_SUCCESS(status)) return status;

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
    pnpCaps.Removable          = WdfTrue;
    pnpCaps.SurpriseRemovalOK  = WdfTrue;
    pnpCaps.UniqueID           = WdfTrue;
    pnpCaps.Address            = Desc->SerialNo;
    pnpCaps.UINumber           = Desc->SerialNo;
    WdfDeviceSetPnpCapabilities(pdo, &pnpCaps);

    WDF_DEVICE_POWER_CAPABILITIES_INIT(&powCaps);
    powCaps.DeviceState[PowerSystemWorking]   = PowerDeviceD0;
    powCaps.DeviceState[PowerSystemSleeping1] = PowerDeviceD2;
    powCaps.DeviceState[PowerSystemSleeping2] = PowerDeviceD2;
    powCaps.DeviceState[PowerSystemSleeping3] = PowerDeviceD2;
    powCaps.DeviceState[PowerSystemHibernate] = PowerDeviceD2;
    powCaps.DeviceState[PowerSystemShutdown]  = PowerDeviceD3;
    powCaps.DeviceD1                          = WdfTrue;
    powCaps.DeviceD2                          = WdfTrue;
    powCaps.WakeFromD0                        = WdfTrue;
    powCaps.WakeFromD1                        = WdfTrue;
    powCaps.WakeFromD2                        = WdfTrue;
    WdfDeviceSetPowerCapabilities(pdo, &powCaps);

    return STATUS_SUCCESS;
}

// =============================================================================
//  SubmitReport - called from the FDO IOCTL handler with a fresh XUSB report.
//  Stamps it into the PDO's cached packet, then completes ONE pending data-IN
//  URB so xusb22 picks the new state up.
// =============================================================================
NTSTATUS
SdBusPdo_SubmitReport(
    _In_ WDFDEVICE          Fdo,
    _In_ PSDBUS_XUSB_REPORT NewReport)
{
    PSDBUS_PDO_CONTEXT pdo = SdBusFdo_FindChildContext(Fdo);
    if (!pdo)
        return STATUS_DEVICE_NOT_READY;

    KIRQL irql;
    KeAcquireSpinLock(&pdo->PacketLock, &irql);
    BOOLEAN changed = (RtlCompareMemory(&pdo->Packet.Report, NewReport,
                          sizeof(SDBUS_XUSB_REPORT)) != sizeof(SDBUS_XUSB_REPORT));
    if (changed)
        RtlCopyMemory(&pdo->Packet.Report, NewReport, sizeof(SDBUS_XUSB_REPORT));
    KeReleaseSpinLock(&pdo->PacketLock, irql);

    // No change -> don't waste a pending IRP. xusb22 polls aggressively.
    if (!changed)
        return STATUS_SUCCESS;

    // Pop one pending IN URB and stuff our packet into it.
    WDFREQUEST req;
    NTSTATUS status = WdfIoQueueRetrieveNextRequest(pdo->PendingDataIn, &req);
    if (!NT_SUCCESS(status))
        return STATUS_SUCCESS;  // no consumer yet; cached packet wins next poll

    PIRP  irp = WdfRequestWdmGetIrp(req);
    PURB  urb = static_cast<PURB>(URB_FROM_IRP(irp));
    auto  buf = static_cast<PUCHAR>(urb->UrbBulkOrInterruptTransfer.TransferBuffer);

    urb->UrbBulkOrInterruptTransfer.TransferBufferLength = sizeof(SDBUS_XUSB_PACKET);
    KeAcquireSpinLock(&pdo->PacketLock, &irql);
    RtlCopyMemory(buf, &pdo->Packet, sizeof(SDBUS_XUSB_PACKET));
    KeReleaseSpinLock(&pdo->PacketLock, irql);

    WdfRequestComplete(req, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}
