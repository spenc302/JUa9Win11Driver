#include <ntddk.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdf.h>
#include <wdfusb.h>

#include "protocol.h"

#define JUA9_VENDOR_ID 0x046D
#define JUA9_PRODUCT_ID 0xC281
#define JUA9_IN_ENDPOINT 0x82
#define JUA9_OUT_ENDPOINT 0x01
#define JUA9_OUT_TRANSFER_SIZE 32

const GUID GUID_DEVINTERFACE_JUA9 =
{
    0x9c1e2c0b, 0x4d72, 0x4a19, {0x9f, 0x6a, 0x7b, 0x4d, 0x11, 0x2e, 0x6c, 0x81}
};

#define IOCTL_JUA9_GET_LAST_REPORT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_JUA9_SEND_FRAME \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_JUA9_QUERY_ID \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _DEVICE_CONTEXT {
    WDFUSBDEVICE UsbDevice;
    WDFUSBPIPE InputPipe;
    WDFUSBPIPE OutputPipe;
    WDFSPINLOCK ReportLock;
    UCHAR LastReport[16];
    UCHAR LastReportLength;
    BOOLEAN Removing;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext);

EVT_WDF_USB_READER_COMPLETION_ROUTINE Jua9EvtReadComplete;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Jua9EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Jua9EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Jua9EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY Jua9EvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT Jua9EvtDeviceD0Exit;
EVT_WDF_USB_READER_COMPLETION_ROUTINE Jua9EvtReadComplete;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Jua9EvtIoDeviceControl;

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    WDF_DRIVER_CONFIG_INIT(&config, Jua9EvtDeviceAdd);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = NULL;
    return WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);
}

NTSTATUS
Jua9EvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware = Jua9EvtPrepareHardware;
    callbacks.EvtDeviceReleaseHardware = Jua9EvtReleaseHardware;
    callbacks.EvtDeviceD0Entry = Jua9EvtDeviceD0Entry;
    callbacks.EvtDeviceD0Exit = Jua9EvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &callbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    NTSTATUS status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(
        device, &GUID_DEVINTERFACE_JUA9, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES lockAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = device;
    status = WdfSpinLockCreate(&lockAttributes, &DeviceGetContext(device)->ReportLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = Jua9EvtIoDeviceControl;
    return WdfIoQueueCreate(
        device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

NTSTATUS
Jua9EvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PDEVICE_CONTEXT context = DeviceGetContext(Device);
    NTSTATUS status;
    WDF_USB_DEVICE_CREATE_CONFIG createConfig;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
    WDFUSBINTERFACE usbInterface;
    UCHAR pipeIndex;

    context->Removing = FALSE;
    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&createConfig,
                                      USBD_CLIENT_CONTRACT_VERSION_602);
    status = WdfUsbTargetDeviceCreateWithParameters(
        Device, &createConfig, WDF_NO_OBJECT_ATTRIBUTES, &context->UsbDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);
    status = WdfUsbTargetDeviceSelectConfig(
        context->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &configParams);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    usbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;
    for (pipeIndex = 0;
         pipeIndex < WdfUsbInterfaceGetNumConfiguredPipes(usbInterface);
         ++pipeIndex) {
        WDFUSBPIPE pipe = WdfUsbInterfaceGetConfiguredPipe(usbInterface, pipeIndex, NULL);
        WDF_USB_PIPE_INFORMATION information;
        WDF_USB_PIPE_INFORMATION_INIT(&information);
        WdfUsbTargetPipeGetInformation(pipe, &information);

        if (information.EndpointAddress == JUA9_IN_ENDPOINT) {
            context->InputPipe = pipe;
        } else if (information.EndpointAddress == JUA9_OUT_ENDPOINT) {
            context->OutputPipe = pipe;
        }
    }

    if (context->InputPipe == NULL || context->OutputPipe == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(context->InputPipe);
    WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(context->OutputPipe);

    WDF_USB_CONTINUOUS_READER_CONFIG readerConfig;
    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &readerConfig,
        Jua9EvtReadComplete,
        Device,
        sizeof(context->LastReport));
    status = WdfUsbTargetPipeConfigContinuousReader(
        context->InputPipe, &readerConfig);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Deliberately read-only: no motor command is sent during startup.
    return STATUS_SUCCESS;
}

NTSTATUS
Jua9EvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PDEVICE_CONTEXT context = DeviceGetContext(Device);

    context->Removing = TRUE;
    if (context->InputPipe != NULL) {
        WdfIoTargetStop(
            WdfUsbTargetPipeGetIoTarget(context->InputPipe),
            WdfIoTargetCancelSentIo);
    }
    if (context->OutputPipe != NULL) {
        WdfIoTargetStop(
            WdfUsbTargetPipeGetIoTarget(context->OutputPipe),
            WdfIoTargetCancelSentIo);
    }
    context->InputPipe = NULL;
    context->OutputPipe = NULL;
    context->UsbDevice = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
Jua9EvtDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    UNREFERENCED_PARAMETER(PreviousState);
    PDEVICE_CONTEXT context = DeviceGetContext(Device);
    NTSTATUS status = STATUS_SUCCESS;

    if (context->InputPipe != NULL) {
        status = WdfIoTargetStart(
            WdfUsbTargetPipeGetIoTarget(context->InputPipe));
    }
    return status;
}

NTSTATUS
Jua9EvtDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    UNREFERENCED_PARAMETER(TargetState);
    PDEVICE_CONTEXT context = DeviceGetContext(Device);

    if (context->InputPipe != NULL) {
        WdfIoTargetStop(
            WdfUsbTargetPipeGetIoTarget(context->InputPipe),
            WdfIoTargetCancelSentIo);
    }
    return STATUS_SUCCESS;
}

VOID
Jua9EvtReadComplete(
    _In_ WDFUSBPIPE Pipe,
    _In_ WDFMEMORY Buffer,
    _In_ size_t NumBytesTransferred,
    _In_ WDFCONTEXT Context
    )
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(Buffer);
    PDEVICE_CONTEXT deviceContext = DeviceGetContext((WDFDEVICE)Context);
    UCHAR command = 0;
    UCHAR length = 0;

    if (!deviceContext->Removing && NumBytesTransferred != 0) {
        BOOLEAN validFrame = Jua9ValidateFrame(
            (PUCHAR)WdfMemoryGetBuffer(Buffer, NULL),
            (ULONG)NumBytesTransferred,
            &command,
            &length);
        WdfSpinLockAcquire(deviceContext->ReportLock);
        RtlCopyMemory(deviceContext->LastReport,
                      WdfMemoryGetBuffer(Buffer, NULL),
                      NumBytesTransferred > sizeof(deviceContext->LastReport)
                          ? sizeof(deviceContext->LastReport)
                          : NumBytesTransferred);
        deviceContext->LastReportLength = (UCHAR)(
            NumBytesTransferred > sizeof(deviceContext->LastReport)
                ? sizeof(deviceContext->LastReport)
                : NumBytesTransferred);
        WdfSpinLockRelease(deviceContext->ReportLock);
        if (validFrame) {
            KdPrint(("JUa9TestDriver: frame command=0x%02X length=%u\n",
                               command, length));
        } else {
            KdPrint(("JUa9TestDriver: raw input length=%Iu\n",
                     NumBytesTransferred));
        }
    }
}

VOID
Jua9EvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    UNREFERENCED_PARAMETER(InputBufferLength);

    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT context = DeviceGetContext(device);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t information = 0;
    ULONG controlTransferLength = 0;

    if (IoControlCode == IOCTL_JUA9_GET_LAST_REPORT) {
        if (OutputBufferLength < sizeof(context->LastReport)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            PUCHAR output = NULL;
            status = WdfRequestRetrieveOutputBuffer(
                Request, sizeof(context->LastReport), (PVOID*)&output, NULL);
            if (NT_SUCCESS(status)) {
                WdfSpinLockAcquire(context->ReportLock);
                RtlZeroMemory(output, sizeof(context->LastReport));
                RtlCopyMemory(output, context->LastReport, context->LastReportLength);
                information = context->LastReportLength;
                WdfSpinLockRelease(context->ReportLock);
            }
        }
    } else if (IoControlCode == IOCTL_JUA9_SEND_FRAME) {
        PUCHAR input = NULL;
        size_t inputLength = 0;
        status = WdfRequestRetrieveInputBuffer(
            Request, 1, (PVOID*)&input, &inputLength);
        if (NT_SUCCESS(status)) {
            /*
             * I-Force USB packets are sent without the older serial
             * 0x2B/length/checksum wrapper. The first byte is the command.
             */
            if (inputLength > JUA9_MAX_PAYLOAD ||
                inputLength == 0 ||
                context->OutputPipe == NULL) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                /*
                 * The real I-Force USB protocol expects exactly the
                 * opcode + payload bytes on the wire, with no padding.
                 * (Linux's iforce-usb.c sets transfer_buffer_length to
                 * exactly n+1 bytes -- never a fixed/padded size.)
                 */
                WDF_MEMORY_DESCRIPTOR descriptor;
                WDF_REQUEST_SEND_OPTIONS options;

                WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
                    &descriptor, input, (ULONG)inputLength);
                WDF_REQUEST_SEND_OPTIONS_INIT(
                    &options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
                WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
                    &options, WDF_REL_TIMEOUT_IN_SEC(1));
                status = WdfUsbTargetPipeWriteSynchronously(
                    context->OutputPipe, NULL, &options,
                    &descriptor, NULL);
            }
        }
    } else if (IoControlCode == IOCTL_JUA9_QUERY_ID) {
        PUCHAR input = NULL;
        PUCHAR output = NULL;
        size_t inputLength = 0;
        size_t outputLength = 0;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(UCHAR), (PVOID*)&input, &inputLength);
        if (NT_SUCCESS(status)) {
            status = WdfRequestRetrieveOutputBuffer(
                Request, 16, (PVOID*)&output, &outputLength);
        }
        if (NT_SUCCESS(status)) {
            WDF_USB_CONTROL_SETUP_PACKET setupPacket;
            WDF_MEMORY_DESCRIPTOR descriptor;
            WDF_REQUEST_SEND_OPTIONS options;
            WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(
                &setupPacket,
                BmRequestDeviceToHost,
                BmRequestToInterface,
                *input,
                0,
                0);
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
                &descriptor, output, outputLength);
            WDF_REQUEST_SEND_OPTIONS_INIT(
                &options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
            WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
                &options, WDF_REL_TIMEOUT_IN_SEC(1));
            status = WdfUsbTargetDeviceSendControlTransferSynchronously(
                context->UsbDevice,
                NULL,
                &options,
                &setupPacket,
                &descriptor,
                &controlTransferLength);
            information = controlTransferLength;
        }
    }

    WdfRequestCompleteWithInformation(Request, status, information);
}
