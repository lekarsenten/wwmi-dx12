#pragma once

typedef int NvAPI_Status;
#define NVAPI_OK 0
#define NVAPI_ERROR (-1)

// Error reporting (nvprofile.cpp log_nv_error).
#define NVAPI_SHORT_STRING_MAX 64
typedef char NvAPI_ShortString[NVAPI_SHORT_STRING_MAX];
inline NvAPI_Status NvAPI_GetErrorMessage(NvAPI_Status /*status*/, NvAPI_ShortString msg)
{
    if (msg) {
        const char* s = "NVAPI not present (DX12 path, no stereo)";
        for (int i = 0; i < NVAPI_SHORT_STRING_MAX - 1 && s[i]; ++i) msg[i] = s[i];
        msg[NVAPI_SHORT_STRING_MAX - 1] = '\0';
    }
    return NVAPI_OK;
}

typedef enum _NVAPI_STEREO_SURFACECREATEMODE {
    NVAPI_STEREO_SURFACECREATEMODE_AUTO = 0,
} NVAPI_STEREO_SURFACECREATEMODE;

typedef unsigned int NvU32;

typedef NvU32 NvAPI_UnicodeString[2048];
typedef struct _NVDRS_SETTING {
    NvU32 version;
    NvU32 settingId;
    NvU32 settingType;
    NvU32 settingLocation;
    NvU32 isCurrentPredefined;
    NvU32 isPredefinedValid;
} NVDRS_SETTING;

typedef struct _NVDRS_PROFILE {
    NvU32 version;
    NvU32 isPredefined;
    NvU32 numOfApps;
} NVDRS_PROFILE;
typedef void* NvDRSSessionHandle;
typedef void* NvDRSProfileHandle;
