//
// XInput.c - kernel-mode bridge from SteamDeckHID to SteamDeckBus.
//
// On every successful translated HID report we additionally push an XUSB
// report to the bus driver via IOCTL_SDBUS_SUBMIT_XUSB. The bus is a
// root-enumerated PnP device named \Device\SteamDeckBus, so we resolve it
// once with IoGetDeviceObjectPointer and reuse the device pointer.
//
// All IRP plumbing is asynchronous fire-and-forget: SubmitReport hands the
// IRP off with our own completion routine that frees the buffer and
// IoFreeIrp(). ReadComplete runs at DISPATCH_LEVEL, so we cannot block
// here; we use IoBuildAsynchronousFsdRequest which is DISPATCH-safe.
//
// If the bus driver isn't loaded (typical on first install before reboot),
// IoGetDeviceObjectPointer returns STATUS_OBJECT_NAME_NOT_FOUND and we
// silently drop submissions until the user reboots. We only retry once
// per HID start to avoid a permanent KdPrint storm.
//
#include "Driver.h"

// ---------------------------------------------------------------------------
// HID button bit positions - mirror src/Report.c for translation lookup.
// ---------------------------------------------------------------------------
#define HID_BTN_A       (1u << 0)
#define HID_BTN_B       (1u << 1)
#define HID_BTN_X       (1u << 2)
#define HID_BTN_Y       (1u << 3)
#define HID_BTN_LB      (1u << 4)
#define HID_BTN_RB      (1u << 5)
#define HID_BTN_VIEW    (1u << 6)
#define HID_BTN_MENU    (1u << 7)
#define HID_BTN_LS      (1u << 8)
#define HID_BTN_RS      (1u << 9)
#define HID_BTN_STEAM   (1u << 14)

// ---------------------------------------------------------------------------
// SteamDeckXInput_Translate - HID report -> XUSB report.
//
// Logic mirrors SteamDeckReport_Translate but produces XInput's wider format.
// L4/L5/R4/R5/QAM have no XInput equivalent; they're dropped here.
// ---------------------------------------------------------------------------
VOID
SteamDeckXInput_Translate(
    _In_  PSTREAMDECK_HID_INPUT_REPORT Hid,
    _Out_ PSDBUS_XUSB_REPORT           Xusb)
{
    USHORT b = Hid->Buttons;
    USHORT x = 0;

    if (b & HID_BTN_A)     x |= SDBUS_XUSB_A;
    if (b & HID_BTN_B)     x |= SDBUS_XUSB_B;
    if (b & HID_BTN_X)     x |= SDBUS_XUSB_X;
    if (b & HID_BTN_Y)     x |= SDBUS_XUSB_Y;
    if (b & HID_BTN_LB)    x |= SDBUS_XUSB_LEFT_SHOULDER;
    if (b & HID_BTN_RB)    x |= SDBUS_XUSB_RIGHT_SHOULDER;
    if (b & HID_BTN_VIEW)  x |= SDBUS_XUSB_BACK;
    if (b & HID_BTN_MENU)  x |= SDBUS_XUSB_START;
    if (b & HID_BTN_LS)    x |= SDBUS_XUSB_LEFT_THUMB;
    if (b & HID_BTN_RS)    x |= SDBUS_XUSB_RIGHT_THUMB;
    if (b & HID_BTN_STEAM) x |= SDBUS_XUSB_GUIDE;

    UCHAR hat = (UCHAR)(Hid->Hat & 0x0F);
    switch (hat) {
    case 0: x |= SDBUS_XUSB_DPAD_UP; break;
    case 1: x |= SDBUS_XUSB_DPAD_UP   | SDBUS_XUSB_DPAD_RIGHT; break;
    case 2: x |= SDBUS_XUSB_DPAD_RIGHT; break;
    case 3: x |= SDBUS_XUSB_DPAD_DOWN | SDBUS_XUSB_DPAD_RIGHT; break;
    case 4: x |= SDBUS_XUSB_DPAD_DOWN; break;
    case 5: x |= SDBUS_XUSB_DPAD_DOWN | SDBUS_XUSB_DPAD_LEFT; break;
    case 6: x |= SDBUS_XUSB_DPAD_LEFT; break;
    case 7: x |= SDBUS_XUSB_DPAD_UP   | SDBUS_XUSB_DPAD_LEFT; break;
    default: break; // 8 = neutral
    }

    Xusb->wButtons      = x;
    Xusb->bLeftTrigger  = Hid->LeftTrigger;
    Xusb->bRightTrigger = Hid->RightTrigger;

    // HID is signed -127..127; XInput wants -32768..32767. <<8 widens it
    // and the existing 2% deadzone in src/Report.c already filtered noise.
    Xusb->sThumbLX = (SHORT)((SHORT)Hid->LeftStickX  << 8);
    Xusb->sThumbLY = (SHORT)((SHORT)Hid->LeftStickY  << 8);
    Xusb->sThumbRX = (SHORT)((SHORT)Hid->RightStickX << 8);
    Xusb->sThumbRY = (SHORT)((SHORT)Hid->RightStickY << 8);
}

// ---------------------------------------------------------------------------
// SteamDeckXInput_Resolve - PASSIVE_LEVEL bus pointer resolution.
//
// IoGetDeviceObjectPointer requires PASSIVE_LEVEL, so we resolve once during
// EvtPrepareHardware and cache the pointer. ReadComplete (DISPATCH_LEVEL) then
// just consumes the cached pointer; no further resolution from there.
//
// BusResolveAttempted is set on the first try (success or fail) so we don't
// retry on every device restart and spam KdPrint when the bus is absent.
// ---------------------------------------------------------------------------
VOID
SteamDeckXInput_Resolve(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    UNICODE_STRING  name;
    NTSTATUS        status;

    PAGED_CODE();

    if (ctx->BusDevice)
        return;

    if (InterlockedCompareExchange(&ctx->BusResolveAttempted, 1, 0) == 1)
        return;

    RtlInitUnicodeString(&name, SDBUS_NT_DEVICE_NAME_W);

    status = IoGetDeviceObjectPointer(
        &name,
        FILE_WRITE_DATA,
        &ctx->BusFileObject,
        &ctx->BusDevice);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "SteamDeckHID: SteamDeckBus not present (0x%X) - XInput disabled\n",
            status));
        ctx->BusDevice     = NULL;
        ctx->BusFileObject = NULL;
        return;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckHID: bound to SteamDeckBus device 0x%p\n", ctx->BusDevice));
}

// ---------------------------------------------------------------------------
// Async IRP completion: free the input buffer + the IRP itself.
// ---------------------------------------------------------------------------
static IO_COMPLETION_ROUTINE _SubmitComplete;

static NTSTATUS
_SubmitComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp,
    _In_ PVOID          Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    if (Context)
        ExFreePoolWithTag(Context, STEAMDECK_POOL_TAG);

    IoFreeIrp(Irp);
    return STATUS_MORE_PROCESSING_REQUIRED;  // we own the IRP now
}

// ---------------------------------------------------------------------------
// SteamDeckXInput_Submit
//
// Fire-and-forget XUSB report submission. Skips when the bus isn't resolved
// or the report is a duplicate of the last one we sent.
// ---------------------------------------------------------------------------
VOID
SteamDeckXInput_Submit(
    _In_ WDFDEVICE          Device,
    _In_ PSDBUS_XUSB_REPORT Report)
{
    PDEVICE_CONTEXT     ctx = DeviceGetContext(Device);
    PIRP                irp;
    PIO_STACK_LOCATION  sl;
    PSDBUS_XUSB_REPORT  buf;
    KIRQL               irql;
    BOOLEAN             changed;

    // Bus pointer is resolved once at PASSIVE during EvtPrepareHardware.
    // If it's still NULL here, the bus driver wasn't loaded - silently drop.
    if (!ctx->BusDevice)
        return;

    // De-dupe in our context so we don't churn IRPs at 250 Hz when nothing
    // moves. The bus driver also de-dupes but we save an IRP allocation.
    KeAcquireSpinLock(&ctx->XusbLock, &irql);
    changed = (RtlCompareMemory(&ctx->LastXusb, Report, sizeof(*Report))
               != sizeof(*Report));
    if (changed)
        RtlCopyMemory(&ctx->LastXusb, Report, sizeof(*Report));
    KeReleaseSpinLock(&ctx->XusbLock, irql);
    if (!changed)
        return;

    buf = (PSDBUS_XUSB_REPORT)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*buf), STEAMDECK_POOL_TAG);
    if (!buf)
        return;
    RtlCopyMemory(buf, Report, sizeof(*buf));

    irp = IoAllocateIrp(ctx->BusDevice->StackSize, FALSE);
    if (!irp) {
        ExFreePoolWithTag(buf, STEAMDECK_POOL_TAG);
        return;
    }

    sl = IoGetNextIrpStackLocation(irp);
    sl->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    sl->Parameters.DeviceIoControl.IoControlCode      = IOCTL_SDBUS_SUBMIT_XUSB;
    sl->Parameters.DeviceIoControl.InputBufferLength  = sizeof(*buf);
    sl->Parameters.DeviceIoControl.OutputBufferLength = 0;
    sl->Parameters.DeviceIoControl.Type3InputBuffer   = NULL;
    sl->FileObject = ctx->BusFileObject;

    // METHOD_BUFFERED reads input from Irp->AssociatedIrp.SystemBuffer.
    // We free both buf and the IRP from the completion routine.
    irp->AssociatedIrp.SystemBuffer = buf;

    IoSetCompletionRoutine(irp, _SubmitComplete, buf, TRUE, TRUE, TRUE);
    (void)IoCallDriver(ctx->BusDevice, irp);
}

// ---------------------------------------------------------------------------
// SteamDeckXInput_Cleanup - drop the bus device reference on D0Exit/release.
// Safe to call when never resolved (BusFileObject == NULL).
// ---------------------------------------------------------------------------
VOID
SteamDeckXInput_Cleanup(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    if (ctx->BusFileObject) {
        ObDereferenceObject(ctx->BusFileObject);
        ctx->BusFileObject = NULL;
        ctx->BusDevice     = NULL;
    }
    InterlockedExchange(&ctx->BusResolveAttempted, 0);
}
