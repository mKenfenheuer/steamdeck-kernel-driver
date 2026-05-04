//
// Usb.c  –  USB initialisation, Lizard Mode control, WDF continuous reader
//
#include "Driver.h"

// ---------------------------------------------------------------------------
// Internal helper – send a Valve proprietary HID Feature Report via
// USB SET_REPORT control transfer (bmRequestType=0x21, bRequest=0x09)
// ---------------------------------------------------------------------------
static NTSTATUS
_SendFeatureReport(
    _In_ WDFDEVICE  Device,
    _In_ UCHAR      ReportId,
    _In_reads_bytes_opt_(PayloadLen) PUCHAR Payload,
    _In_ ULONG      PayloadLen)
{
    PDEVICE_CONTEXT              ctx = DeviceGetContext(Device);
    NTSTATUS                     status;
    WDF_USB_CONTROL_SETUP_PACKET setup;
    WDF_MEMORY_DESCRIPTOR        memDesc;
    WDF_REQUEST_SEND_OPTIONS     sendOptions;
    UCHAR                        buf[65];
    ULONG                        transferLen;

    if (PayloadLen > sizeof(buf) - 1)
        PayloadLen = sizeof(buf) - 1;

    RtlZeroMemory(buf, sizeof(buf));
    buf[0] = ReportId;
    if (Payload && PayloadLen > 0)
        RtlCopyMemory(&buf[1], Payload, PayloadLen);

    // Send EXACTLY (report-id + payload) bytes. The Steam Deck firmware
    // expects wLength to match the feature report's declared size; sending
    // a fixed 65-byte transfer for a 2- or 4-byte command stalls the
    // control endpoint and hangs the device.
    transferLen = 1 + (ULONG)PayloadLen;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memDesc, buf, transferLen);

    WDF_USB_CONTROL_SETUP_PACKET_INIT_CLASS(
        &setup,
        BmRequestHostToDevice,
        BmRequestToInterface,
        0x09,                                               // SET_REPORT
        (USHORT)((0x03 << 8) | ReportId),                  // Feature | ID
        (USHORT)WdfUsbInterfaceGetInterfaceNumber(ctx->UsbInterface));

    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&sendOptions, WDF_REL_TIMEOUT_IN_MS(3000));

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        ctx->UsbDevice, WDF_NO_HANDLE, &sendOptions, &setup, &memDesc, NULL);

    if (!NT_SUCCESS(status))
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "SteamDeckHID: Feature report 0x%02X failed 0x%X\n",
            ReportId, status));

    return status;
}

// ---------------------------------------------------------------------------
// SteamDeckUsb_Init
// Opens the WDFUSBDEVICE, selects configuration, finds interface 2 and pipe.
// ---------------------------------------------------------------------------
NTSTATUS
SteamDeckUsb_Init(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT                     ctx = DeviceGetContext(Device);
    NTSTATUS                            status;
    WDF_USB_DEVICE_CREATE_CONFIG        usbCfg;
    WDFUSBDEVICE                        usbDev;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS selCfg;
    UCHAR                               numIfaces, numPipes, i, p;
    WDF_USB_PIPE_INFORMATION            pipeInfo;

    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&usbCfg, USBD_CLIENT_CONTRACT_VERSION_602);
    status = WdfUsbTargetDeviceCreateWithParameters(
        Device, &usbCfg, WDF_NO_OBJECT_ATTRIBUTES, &usbDev);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: WdfUsbTargetDeviceCreateWithParameters 0x%X\n", status));
        return status;
    }
    ctx->UsbDevice = usbDev;

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(&selCfg, 0, NULL);
    status = WdfUsbTargetDeviceSelectConfig(usbDev, WDF_NO_OBJECT_ATTRIBUTES, &selCfg);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: SelectConfig 0x%X\n", status));
        return status;
    }

    // Find interface 2 (gamepad: bInterfaceProtocol == 0, bInterfaceClass == 3)
    numIfaces = WdfUsbTargetDeviceGetNumInterfaces(usbDev);
    ctx->UsbInterface = NULL;
    for (i = 0; i < numIfaces; i++) {
        WDFUSBINTERFACE     iface = WdfUsbTargetDeviceGetInterface(usbDev, i);
        USB_INTERFACE_DESCRIPTOR ifDesc;
        WdfUsbInterfaceGetDescriptor(iface, 0, &ifDesc);
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "SteamDeckHID: iface[%d] class=%d proto=%d\n",
            i, ifDesc.bInterfaceClass, ifDesc.bInterfaceProtocol));
        if (ifDesc.bInterfaceClass == 3 && ifDesc.bInterfaceProtocol == 0) {
            ctx->UsbInterface = iface;
            break;
        }
    }
    if (!ctx->UsbInterface) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: gamepad interface not found\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    // Find Interrupt IN pipe
    numPipes = WdfUsbInterfaceGetNumConfiguredPipes(ctx->UsbInterface);
    ctx->InterruptInPipe = NULL;
    for (p = 0; p < numPipes; p++) {
        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
        WDFUSBPIPE pipe =
            WdfUsbInterfaceGetConfiguredPipe(ctx->UsbInterface, p, &pipeInfo);
        if (pipeInfo.PipeType == WdfUsbPipeTypeInterrupt &&
            WdfUsbTargetPipeIsInEndpoint(pipe)) {
            ctx->InterruptInPipe = pipe;
            WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "SteamDeckHID: Interrupt IN pipe found, MaxPacket=%d\n",
                pipeInfo.MaximumPacketSize));
            break;
        }
    }
    if (!ctx->InterruptInPipe) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: Interrupt IN pipe not found\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// SteamDeckUsb_DisableLizardMode
//
// Sends the two Valve feature reports that switch the device out of keyboard+
// mouse emulation. Payloads are taken verbatim from neptune-hidapi.net's
// SetLizardMode(false, false):
//
//   PT_CLEAR_MAPPINGS (0x81) + [0x00]                -> disable button emulation
//   PT_CONFIGURE     (0x87) + [0x03, 0x08, 0x07]    -> disable mouse  emulation
//
// The device re-enables Lizard mode on its own, so a periodic timer
// (DEVICE_CONTEXT::LizardTimer, 250 ms) keeps re-issuing this.
// ---------------------------------------------------------------------------
NTSTATUS
SteamDeckUsb_DisableLizardMode(_In_ WDFDEVICE Device)
{
    NTSTATUS status;
    UCHAR    clearPayload[]    = { 0x00 };
    UCHAR    disableMousePayload[] = { 0x03, 0x08, 0x07 };

    status = _SendFeatureReport(Device, NEPTUNE_PT_CLEAR_MAPPINGS,
                                clearPayload, sizeof(clearPayload));
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "SteamDeckHID: Lizard buttons disable failed 0x%X\n", status));
        return status;
    }

    status = _SendFeatureReport(Device, NEPTUNE_PT_CONFIGURE,
                                disableMousePayload, sizeof(disableMousePayload));
    if (!NT_SUCCESS(status))
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "SteamDeckHID: Lizard mouse disable failed 0x%X\n", status));

    return status;
}

// ---------------------------------------------------------------------------
// SteamDeckUsb_EnableLizardMode
//
// Re-enables keyboard + mouse emulation. Mirror of DisableLizardMode using
// the inverse C# commands from neptune-hidapi.net's SetLizardMode(true,true):
//
//   PT_LIZARD_BUTTONS (0x85) + [0x00]   -> ENABLE button emulation
//   PT_LIZARD_MOUSE   (0x8E) + [0x00]   -> ENABLE mouse emulation
//
// Used by the L3+R3 chord toggle when LizardModeEnabled becomes TRUE.
// ---------------------------------------------------------------------------
NTSTATUS
SteamDeckUsb_EnableLizardMode(_In_ WDFDEVICE Device)
{
    NTSTATUS status;
    UCHAR    payload[] = { 0x00 };

    status = _SendFeatureReport(Device, NEPTUNE_PT_LIZARD_BUTTONS,
                                payload, sizeof(payload));
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "SteamDeckHID: Lizard buttons enable failed 0x%X\n", status));
        return status;
    }

    status = _SendFeatureReport(Device, NEPTUNE_PT_LIZARD_MOUSE,
                                payload, sizeof(payload));
    if (!NT_SUCCESS(status))
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "SteamDeckHID: Lizard mouse enable failed 0x%X\n", status));

    return status;
}

// ---------------------------------------------------------------------------
// SteamDeckUsb_ReadComplete  –  WDF continuous reader callback (~250 Hz)
//
// Per USB report:
//   1. Translate raw -> intermediate gamepad layout (same struct used to be
//      our HID report; now it's just an internal staging form on the way
//      to the XUSB report sent to SteamDeckBus).
//   2. Run the L3+R3 hold-3s chord state machine. If it fires, flip
//      LizardModeEnabled and kick the work item to apply on the device.
//   3. If LizardModeEnabled, replace the report with neutral so games see
//      "no input" while keyboard/mouse mode is active.
//   4. Submit the XUSB report to SteamDeckBus (no-op if bus not loaded).
// ---------------------------------------------------------------------------

// 3 seconds expressed in 100 ns units (KeQueryInterruptTime granularity).
#define LIZARD_CHORD_HOLD_100NS  (3ULL * 10000000ULL)

VOID
SteamDeckUsb_ReadComplete(
    _In_ WDFUSBPIPE  Pipe,
    _In_ WDFMEMORY   Buffer,
    _In_ size_t      NumBytesTransferred,
    _In_ WDFCONTEXT  Context)
{
    WDFDEVICE                  device = (WDFDEVICE)Context;
    PDEVICE_CONTEXT            ctx    = DeviceGetContext(device);
    KIRQL                      irql;
    STEAMDECK_HID_INPUT_REPORT translated;
    BOOLEAN                    chord;
    BOOLEAN                    shouldKickWorkItem = FALSE;
    ULONGLONG                  now;

    UNREFERENCED_PARAMETER(Pipe);

    if (NumBytesTransferred < sizeof(NEPTUNE_RAW_REPORT))
        return;

    PNEPTUNE_RAW_REPORT raw =
        (PNEPTUNE_RAW_REPORT)WdfMemoryGetBuffer(Buffer, NULL);

    if (raw->PacketType != NEPTUNE_REPORT_ID_INPUT)
        return;

    SteamDeckReport_Translate(raw, &translated);

    // L3+R3 detection: stick clicks live in different button bytes per the
    // Neptune layout (LSTICK_PRESS in Buttons1, RSTICK_PRESS in Buttons2).
    chord = (raw->Buttons1 & NEPTUNE_BTN1_LSTICK_PRESS) != 0 &&
            (raw->Buttons2 & NEPTUNE_BTN2_RSTICK_PRESS) != 0;

    now = KeQueryInterruptTime();

    KeAcquireSpinLock(&ctx->ReportLock, &irql);

    if (chord) {
        if (ctx->ChordStartTime == 0) {
            ctx->ChordStartTime = now;
            ctx->ChordConsumed  = FALSE;
        } else if (!ctx->ChordConsumed &&
                   (now - ctx->ChordStartTime) >= LIZARD_CHORD_HOLD_100NS) {
            ctx->ChordConsumed   = TRUE;
            ctx->LizardModeEnabled = !ctx->LizardModeEnabled;
            shouldKickWorkItem   = TRUE;
        }
    } else {
        ctx->ChordStartTime = 0;
        ctx->ChordConsumed  = FALSE;
    }

    // Mute when Lizard is on: neutral report so games see no input.
    if (ctx->LizardModeEnabled) {
        RtlZeroMemory(&translated, sizeof(translated));
        translated.ReportId = NEPTUNE_REPORT_ID_INPUT;
        translated.Hat      = 8;   // neutral
    }

    KeReleaseSpinLock(&ctx->ReportLock, irql);

    // Apply the new state on the device immediately (PASSIVE_LEVEL via
    // the work item). Safe to call at DISPATCH; no-op if already pending.
    if (shouldKickWorkItem && ctx->LizardWorkItem) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "SteamDeckHID: Lizard toggle -> %s\n",
            ctx->LizardModeEnabled ? "ON (muted)" : "OFF (gamepad)"));
        WdfWorkItemEnqueue(ctx->LizardWorkItem);
    }

    // Push the report to SteamDeckBus so XInput games see it. Skipped
    // automatically if the bus driver isn't loaded (BusDevice == NULL).
    // When muted by Lizard mode, `translated` is already neutral.
    {
        SDBUS_XUSB_REPORT xusb;
        SteamDeckXInput_Translate(&translated, &xusb);
        SteamDeckXInput_Submit(device, &xusb);
    }
}

// ---------------------------------------------------------------------------
// SteamDeckUsb_ReadError  –  return TRUE to reset pipe and retry
// ---------------------------------------------------------------------------
BOOLEAN
SteamDeckUsb_ReadError(
    _In_ WDFUSBPIPE  Pipe,
    _In_ NTSTATUS    Status,
    _In_ USBD_STATUS UsbdStatus)
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(Status);
    UNREFERENCED_PARAMETER(UsbdStatus);
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
        "SteamDeckHID: ReadError 0x%X usbd=0x%X – retrying\n",
        Status, UsbdStatus));
    return TRUE;
}

// ---------------------------------------------------------------------------
// SteamDeckUsb_StartReader  –  configure + start WDF continuous reader
// ---------------------------------------------------------------------------
NTSTATUS
SteamDeckUsb_StartReader(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT                   ctx = DeviceGetContext(Device);
    NTSTATUS                          status;
    WDF_USB_CONTINUOUS_READER_CONFIG  cfg;

    if (ctx->ReaderActive)
        return STATUS_SUCCESS;

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &cfg, SteamDeckUsb_ReadComplete, Device, NEPTUNE_INPUT_REPORT_SIZE);

    cfg.EvtUsbTargetPipeReadersFailed = SteamDeckUsb_ReadError;
    cfg.NumPendingReads               = 2;   // double-buffered

    status = WdfUsbTargetPipeConfigContinuousReader(ctx->InterruptInPipe, &cfg);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: ConfigContinuousReader 0x%X\n", status));
        return status;
    }

    status = WdfIoTargetStart(
        WdfUsbTargetPipeGetIoTarget(ctx->InterruptInPipe));

    if (NT_SUCCESS(status)) {
        ctx->ReaderActive = TRUE;
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "SteamDeckHID: Continuous reader started\n"));
    } else {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: WdfIoTargetStart 0x%X\n", status));
    }
    return status;
}

// ---------------------------------------------------------------------------
// Periodic Lizard-mode-disable timer + work item
//
// The Steam Deck firmware re-enables Lizard mode on its own, so we re-send
// the disable feature reports every 250 ms (mirrors neptune-hidapi.net's
// ConfigureLoop).
//
// USB control transfers must run at PASSIVE_LEVEL, but a PASSIVE-level
// WDFTIMER requires its parent WDFDEVICE to also be PASSIVE -- and forcing
// the device to PASSIVE would make every IOCTL queue callback defer through
// a worker thread, slowing the input hot path. So we use the standard
// pattern: a DISPATCH-level periodic timer (no parent constraint) whose
// only job is to enqueue a WDFWORKITEM. Work items always run at PASSIVE.
//
// WdfWorkItemEnqueue is a no-op if the item is already pending, so we
// naturally coalesce if the USB transfer ever takes longer than one tick.
// ---------------------------------------------------------------------------
#define LIZARD_PERIOD_MS    250

EVT_WDF_WORKITEM SteamDeckUsb_LizardWorkItemFunc;

VOID
SteamDeckUsb_LizardWorkItemFunc(_In_ WDFWORKITEM WorkItem)
{
    WDFDEVICE       device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PDEVICE_CONTEXT ctx    = DeviceGetContext(device);

    // Atomic 1-byte read; no lock needed. Whichever value is current at run
    // time is what gets applied -- intermediate toggles are naturally
    // coalesced (we re-assert every 250 ms regardless).
    if (ctx->LizardModeEnabled)
        (VOID)SteamDeckUsb_EnableLizardMode(device);
    else
        (VOID)SteamDeckUsb_DisableLizardMode(device);
}

VOID
SteamDeckUsb_LizardTimerFunc(_In_ WDFTIMER Timer)
{
    WDFDEVICE       device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PDEVICE_CONTEXT ctx    = DeviceGetContext(device);

    if (ctx->LizardWorkItem)
        WdfWorkItemEnqueue(ctx->LizardWorkItem);
}

NTSTATUS
SteamDeckUsb_CreateLizardTimer(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT       ctx = DeviceGetContext(Device);
    WDF_TIMER_CONFIG      timerCfg;
    WDF_WORKITEM_CONFIG   workCfg;
    WDF_OBJECT_ATTRIBUTES attrs;
    NTSTATUS              status;

    // Work item first: must exist before the timer can enqueue it.
    WDF_WORKITEM_CONFIG_INIT(&workCfg, SteamDeckUsb_LizardWorkItemFunc);
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = Device;

    status = WdfWorkItemCreate(&workCfg, &attrs, &ctx->LizardWorkItem);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: WdfWorkItemCreate(Lizard) 0x%X\n", status));
        return status;
    }

    // Timer at default (DISPATCH) level -- no parent ExecutionLevel constraint.
    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerCfg,
                                   SteamDeckUsb_LizardTimerFunc,
                                   LIZARD_PERIOD_MS);
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = Device;

    status = WdfTimerCreate(&timerCfg, &attrs, &ctx->LizardTimer);
    if (!NT_SUCCESS(status))
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "SteamDeckHID: WdfTimerCreate(Lizard) 0x%X\n", status));

    return status;
}

VOID
SteamDeckUsb_StartLizardTimer(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    if (!ctx->LizardTimer) return;
    // Fire immediately, then auto-repeat every LIZARD_PERIOD_MS.
    WdfTimerStart(ctx->LizardTimer, 0);
}

VOID
SteamDeckUsb_StopLizardTimer(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    if (ctx->LizardTimer)
        WdfTimerStop(ctx->LizardTimer, TRUE);
    if (ctx->LizardWorkItem)
        WdfWorkItemFlush(ctx->LizardWorkItem);
}

// ---------------------------------------------------------------------------
// SteamDeckUsb_StopReader
// ---------------------------------------------------------------------------
VOID
SteamDeckUsb_StopReader(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    // Guard against teardown when PrepareHardware never finished.
    // Without this, WdfUsbTargetPipeGetIoTarget(NULL) faults.
    if (!ctx->ReaderActive || !ctx->InterruptInPipe) return;

    WdfIoTargetStop(
        WdfUsbTargetPipeGetIoTarget(ctx->InterruptInPipe),
        WdfIoTargetCancelSentIo);

    ctx->ReaderActive = FALSE;
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "SteamDeckHID: Continuous reader stopped\n"));
}
