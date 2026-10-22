// Shared IPC contract between the Switchify sysmodule and its Tesla overlay.
#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCHIFY_SERVICE_NAME "switchfy"  // sm service names are max 8 chars
#define SWITCHIFY_TITLE_ID     0x4200000000005107ULL
#define SWITCHIFY_IPC_VERSION  3

typedef enum {
    SwitchifyCmd_GetVersion   = 0,  // out: u32
    SwitchifyCmd_GetStatus    = 1,  // out buffer: SwitchifyStatus
    SwitchifyCmd_PlayPause    = 2,
    SwitchifyCmd_Next         = 3,
    SwitchifyCmd_Prev         = 4,
    SwitchifyCmd_SetVolume    = 5,  // in: u32 (0-100)
    SwitchifyCmd_Logout       = 6,  // forget stored credentials, return to pairing
    SwitchifyCmd_Seek         = 7,  // in: u32 position in ms
    SwitchifyCmd_GetImageUrl  = 8,  // out buffer: char[SWITCHIFY_URL_LEN], album art (v3+)
} SwitchifyCmd;

#define SWITCHIFY_URL_LEN 256

typedef enum {
    SwitchifyState_Starting       = 0,
    SwitchifyState_NoNetwork      = 1,
    SwitchifyState_WaitingPairing = 2,  // advertising over mDNS, waiting for phone
    SwitchifyState_Connecting     = 3,
    SwitchifyState_Idle           = 4,  // logged in, nothing loaded
    SwitchifyState_Playing        = 5,
    SwitchifyState_Paused         = 6,
    SwitchifyState_Error          = 7,
} SwitchifyState;

typedef struct {
    u32  state;        // SwitchifyState
    u32  volume;       // 0-100
    u32  position_ms;
    u32  duration_ms;
    char device_name[64];
    char ip[16];
    char username[64];
    char title[128];
    char artist[128];
    char album[128];
    char message[128];  // last error / hint
} SwitchifyStatus;

#ifdef __cplusplus
}
#endif
