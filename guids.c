/*
 * guids.c - Explicit definitions of the COM GUIDs and property keys that
 *           the WASAPI / MMDevice headers declare but no import library
 *           exports for C builds.
 *
 * In C++ these symbols are supplied via __uuidof / MIDL-generated headers.
 * In a pure C build the SDK headers only declare them (EXTERN_C const ...),
 * so we must define them once in our own translation unit.  The values are
 * the documented, stable interface/class identifiers.
 *
 * Media Foundation GUIDs (MFMediaType_*, MFAudioFormat_*, MF_MT_*) are NOT
 * defined here - they are linked from mfuuid.lib.
 */
#include "common.h"

/* mmdeviceapi.h */
EXTERN_C const CLSID CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };

EXTERN_C const IID IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };

/* audioclient.h */
EXTERN_C const IID IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };

EXTERN_C const IID IID_IAudioCaptureClient =
    { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

/* functiondiscoverykeys_devpkey.h
 * PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, 14 */
EXTERN_C const PROPERTYKEY PKEY_Device_FriendlyName =
    { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };
