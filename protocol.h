#pragma once

#ifndef _WINDOWS_
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#endif

#ifndef STATUS_SUCCESS
#include <ntstatus.h>
#endif

#define JUA9_HOST_SYNC 0x2B
#define JUA9_DEVICE_SYNC 0x2C
#define JUA9_MAX_PAYLOAD 16

#define JUA9_CMD_SET_EFFECT 0x01
#define JUA9_CMD_SET_ENVELOPE 0x02
#define JUA9_CMD_SET_CONSTANT 0x03
#define JUA9_CMD_SET_SPRING 0x04
#define JUA9_CMD_SET_CONDITION 0x05
#define JUA9_CMD_SET_GAIN 0x0B
#define JUA9_CMD_CONTROL_EFFECT 0x11
#define JUA9_CMD_QUERY_ID 0x40

typedef struct _JUA9_FRAME {
    UCHAR Sync;
    UCHAR Command;
    UCHAR Length;
    UCHAR Payload[JUA9_MAX_PAYLOAD];
    UCHAR Checksum;
} JUA9_FRAME, *PJUA9_FRAME;

NTSTATUS
Jua9BuildFrame(
    _Out_writes_bytes_(JUA9_MAX_PAYLOAD + 4) PUCHAR Buffer,
    _In_ UCHAR Command,
    _In_reads_bytes_(Length) const UCHAR* Payload,
    _In_ UCHAR Length
    );

BOOLEAN
Jua9ValidateFrame(
    _In_reads_bytes_(BufferLength) const UCHAR* Buffer,
    _In_ ULONG BufferLength,
    _Out_opt_ PUCHAR Command,
    _Out_opt_ PUCHAR Length
    );
