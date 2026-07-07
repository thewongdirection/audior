/*
 * device.h - Audio endpoint enumeration and selection (MMDevice API).
 */
#ifndef AUDIOR_DEVICE_H
#define AUDIOR_DEVICE_H

#include "common.h"

/* Print all active capture and render endpoints, marking the system default
 * for each category with [DEFAULT]. */
HRESULT Device_ListAll(void);

/*
 * Resolve a device for the given data flow (eCapture or eRender).
 * `selector` may be:
 *   - empty / NULL   -> the system default endpoint,
 *   - a numeric index into the active endpoint list for that flow,
 *   - a substring of the endpoint ID (exact GUID portion), or
 *   - a case-insensitive substring of the friendly name.
 * Returns a referenced IMMDevice in *ppDevice on success.
 */
HRESULT Device_Select(EDataFlow flow, const wchar_t *selector, IMMDevice **ppDevice);

/* Copy the endpoint friendly name into `buf` (cch characters). */
HRESULT Device_GetFriendlyName(IMMDevice *device, wchar_t *buf, size_t cch);

#endif /* AUDIOR_DEVICE_H */
