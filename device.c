#include "device.h"

#include <wchar.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Case-insensitive wide substring search.
 *
 * Used for matching a device by (part of) its friendly name.  The CRT has no
 * wide case-insensitive strstr, so we scan with _wcsnicmp.  Friendly names are
 * short and this runs only during device selection, so the O(n*m) cost is
 * irrelevant. */
static const wchar_t *WcsIStr(const wchar_t *haystack, const wchar_t *needle)
{
    size_t nlen = wcslen(needle);
    if (nlen == 0)
        return haystack;

    for (; *haystack; ++haystack) {
        if (_wcsnicmp(haystack, needle, nlen) == 0)
            return haystack;
    }
    return NULL;
}

static BOOL IsAllDigits(const wchar_t *s)
{
    if (!s || !*s)
        return FALSE;
    for (; *s; ++s) {
        if (*s < L'0' || *s > L'9')
            return FALSE;
    }
    return TRUE;
}

HRESULT Device_GetFriendlyName(IMMDevice *device, wchar_t *buf, size_t cch)
{
    IPropertyStore *store = NULL;
    PROPVARIANT     pv;
    HRESULT         hr;

    PropVariantInit(&pv);
    buf[0] = L'\0';

    hr = IMMDevice_OpenPropertyStore(device, STGM_READ, &store);
    if (FAILED(hr))
        return hr;

    hr = IPropertyStore_GetValue(store, &PKEY_Device_FriendlyName, &pv);
    if (SUCCEEDED(hr) && pv.vt == VT_LPWSTR && pv.pwszVal)
        wcsncpy_s(buf, cch, pv.pwszVal, _TRUNCATE);

    PropVariantClear(&pv);
    SAFE_RELEASE(store);
    return hr;
}

/* Return the ID string of the default endpoint for `flow`, or NULL.
 * Caller frees with CoTaskMemFree. */
static wchar_t *GetDefaultId(IMMDeviceEnumerator *en, EDataFlow flow)
{
    IMMDevice *dev = NULL;
    wchar_t   *id  = NULL;

    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, flow, eConsole, &dev))) {
        IMMDevice_GetId(dev, &id);
        SAFE_RELEASE(dev);
    }
    return id;
}

/* ------------------------------------------------------------------ */
/* Listing                                                             */
/* ------------------------------------------------------------------ */

static void PrintCollection(IMMDeviceEnumerator *en, EDataFlow flow, const char *title)
{
    IMMDeviceCollection *coll       = NULL;
    wchar_t             *defaultId  = NULL;
    UINT                 count      = 0;
    UINT                 i;

    printf("\n%s:\n", title);

    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(
            en, flow, DEVICE_STATE_ACTIVE, &coll))) {
        printf("  (unable to enumerate)\n");
        return;
    }

    defaultId = GetDefaultId(en, flow);
    IMMDeviceCollection_GetCount(coll, &count);

    if (count == 0)
        printf("  (none)\n");

    for (i = 0; i < count; ++i) {
        IMMDevice *dev = NULL;
        wchar_t   *id  = NULL;
        wchar_t    name[256];
        BOOL       isDefault;

        if (FAILED(IMMDeviceCollection_Item(coll, i, &dev)))
            continue;

        IMMDevice_GetId(dev, &id);
        Device_GetFriendlyName(dev, name, 256);
        isDefault = (defaultId && id && wcscmp(defaultId, id) == 0);

        wprintf(L"  [%u] %ls%ls\n", i, name, isDefault ? L"  [DEFAULT]" : L"");
        wprintf(L"      ID: %ls\n", id ? id : L"(unknown)");

        CoTaskMemFree(id);
        SAFE_RELEASE(dev);
    }

    CoTaskMemFree(defaultId);
    SAFE_RELEASE(coll);
}

HRESULT Device_ListAll(void)
{
    IMMDeviceEnumerator *en = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to create device enumerator (hr=0x%08lX)\n",
                (unsigned long)hr);
        return hr;
    }

    printf("=== Audio Devices ===\n");
    PrintCollection(en, eCapture, "Microphones / Capture Devices");
    PrintCollection(en, eRender,  "Speakers / Playback Devices");
    printf("\n");

    SAFE_RELEASE(en);
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* Selection                                                           */
/* ------------------------------------------------------------------ */

/* Find a device in the active collection for `flow` matching `selector`. */
static HRESULT SelectFromCollection(IMMDeviceEnumerator *en, EDataFlow flow,
                                    const wchar_t *selector, IMMDevice **ppDevice)
{
    IMMDeviceCollection *coll  = NULL;
    UINT                 count = 0;
    UINT                 i;
    HRESULT              hr;
    BOOL                 byIndex = IsAllDigits(selector);
    UINT                 wantIndex = byIndex ? (UINT)_wtoi(selector) : 0;

    hr = IMMDeviceEnumerator_EnumAudioEndpoints(en, flow, DEVICE_STATE_ACTIVE, &coll);
    if (FAILED(hr))
        return hr;

    IMMDeviceCollection_GetCount(coll, &count);

    for (i = 0; i < count; ++i) {
        IMMDevice *dev = NULL;
        wchar_t   *id  = NULL;
        wchar_t    name[256];
        BOOL       match = FALSE;

        if (FAILED(IMMDeviceCollection_Item(coll, i, &dev)))
            continue;

        if (byIndex) {
            match = (i == wantIndex);
        } else {
            IMMDevice_GetId(dev, &id);
            Device_GetFriendlyName(dev, name, 256);
            /* Device IDs are opaque, case-exact strings, so match them
             * case-sensitively (wcsstr).  Friendly names are for humans, so
             * match them case-insensitively (WcsIStr) for convenience. */
            match = (id && wcsstr(id, selector) != NULL) ||
                    WcsIStr(name, selector) != NULL;
            CoTaskMemFree(id);
        }

        if (match) {
            *ppDevice = dev;          /* transfer reference to caller */
            SAFE_RELEASE(coll);
            return S_OK;
        }
        SAFE_RELEASE(dev);
    }

    SAFE_RELEASE(coll);
    return E_INVALIDARG;
}

HRESULT Device_Select(EDataFlow flow, const wchar_t *selector, IMMDevice **ppDevice)
{
    IMMDeviceEnumerator *en = NULL;
    HRESULT hr;

    *ppDevice = NULL;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr))
        return hr;

    if (!selector || selector[0] == L'\0')
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, flow, eConsole, ppDevice);
    else
        hr = SelectFromCollection(en, flow, selector, ppDevice);

    SAFE_RELEASE(en);
    return hr;
}
