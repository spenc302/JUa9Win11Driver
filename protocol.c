#include "protocol.h"

NTSTATUS
Jua9BuildFrame(
    _Out_writes_bytes_(JUA9_MAX_PAYLOAD + 4) PUCHAR Buffer,
    _In_ UCHAR Command,
    _In_reads_bytes_(Length) const UCHAR* Payload,
    _In_ UCHAR Length
    )
{
    UCHAR checksum = 0;

    if (Buffer == NULL || (Length != 0 && Payload == NULL) ||
        Length > JUA9_MAX_PAYLOAD) {
        return STATUS_INVALID_PARAMETER;
    }

    Buffer[0] = JUA9_HOST_SYNC;
    Buffer[1] = Command;
    Buffer[2] = Length;

    for (UCHAR i = 0; i < Length; ++i) {
        Buffer[3 + i] = Payload[i];
        checksum = (UCHAR)(checksum + Payload[i]);
    }

    Buffer[3 + Length] = checksum;
    return STATUS_SUCCESS;
}

BOOLEAN
Jua9ValidateFrame(
    _In_reads_bytes_(BufferLength) const UCHAR* Buffer,
    _In_ ULONG BufferLength,
    _Out_opt_ PUCHAR Command,
    _Out_opt_ PUCHAR Length
    )
{
    UCHAR checksum = 0;

    if (Buffer == NULL || BufferLength < 4 || Buffer[0] != JUA9_DEVICE_SYNC ||
        Buffer[2] > JUA9_MAX_PAYLOAD ||
        BufferLength < (ULONG)Buffer[2] + 4) {
        return FALSE;
    }

    for (UCHAR i = 0; i < Buffer[2]; ++i) {
        checksum = (UCHAR)(checksum + Buffer[3 + i]);
    }

    if (checksum != Buffer[3 + Buffer[2]]) {
        return FALSE;
    }

    if (Command != NULL) {
        *Command = Buffer[1];
    }
    if (Length != NULL) {
        *Length = Buffer[2];
    }
    return TRUE;
}
