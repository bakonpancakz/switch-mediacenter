// Which way up the console is held, for the viewers that turn with it.
//
// It reads the motion sensor of the Joy-Cons on the console's rails (the
// handheld controller, or a Switch Lite's built-in one): they turn with the
// screen. libnx has no documented reading of the console's own sensor (its
// SevenSixAxisSensor state is unnamed fields), so with the Joy-Cons off, or
// docked, nothing turns.
//
// All four ways round are told apart, from the pull of gravity in the screen's
// own plane (x and y), so holding it tilted back doesn't matter; lying flat
// keeps whatever it was. Checked on the console: a turn to the left (left edge
// down) reads as -x. Which sign of y is "the top of the screen is up" is settled
// by how it's held: whichever way round it spends more time in landscape is up
// (upside down is only ever brief). Until that's clear, both count as landscape.
#include "app.h"
#include <cmath>
#include <cstring>

static HidSixAxisSensorHandle s_handle;
static bool s_ok = false;
static int s_rot = 0, s_cand = 0;   // shown, and the one the console has been held at since s_candAt
static uint32_t s_candAt = 0;
static float s_upTime = 0;          // seconds held landscape with +y up, minus those with -y up
static HidVector s_lastAcc = {};    // the last reading, and since when it's been exactly that
static uint32_t s_sameSince = 0, s_restartAt = 0;
static int s_savedUp = 0;

static bool startSensor() {
    Result rc = hidGetSixAxisSensorHandles(&s_handle, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    if (R_SUCCEEDED(rc)) rc = hidStartSixAxisSensor(s_handle);
    if (R_FAILED(rc)) appLog("orientation: couldn't start the motion sensor (0x%x)", rc);
    return R_SUCCEEDED(rc);
}

void orientationInit() {
    s_ok = startSensor();
    if (!s_ok) appLog("orientation: no motion sensor, the viewers won't turn");
    s_upTime = cfgInt("display.up_time", 0) / 10.0f;
    s_savedUp = (int)s_upTime;
}

// Back from the HOME menu (or sleep): the sensor was stopped meanwhile.
void orientationResume() {
    if (!s_ok) return;
    hidStopSixAxisSensor(s_handle);
    startSensor();
    s_sameSince = g_ticks;
    appLog("orientation: motion sensor restarted (back in the foreground)");
}

void orientationExit() {
    if (s_ok) hidStopSixAxisSensor(s_handle);
    s_ok = false;
}

int deviceRotation() { return s_rot; }

void orientationTick() {
    bool usable = s_ok && cfgBool("display.autorotate", true) && appletGetOperationMode() == AppletOperationMode_Handheld &&
                  (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld);
    HidSixAxisSensorState st;
    if (!usable || hidGetSixAxisSensorStates(s_handle, &st, 1) < 1) {
        s_rot = s_cand = 0;
        return;
    }
    // A sensor that stopped some other way reads exactly the same forever (a
    // live one always jitters in the last digits): after 2 s of that, start it
    // again, at most every 10 s. (Watching its sampling number instead broke
    // turning altogether.)
    if (memcmp(&st.acceleration, &s_lastAcc, sizeof(s_lastAcc)) != 0) {
        s_lastAcc = st.acceleration;
        s_sameSince = g_ticks;
    } else if (g_ticks - s_sameSince > 2000 && g_ticks - s_restartAt > 10000) {
        s_restartAt = g_ticks;
        appLog("orientation: the motion sensor froze (x %.3f, y %.3f, z %.3f), starting it again", st.acceleration.x,
               st.acceleration.y, st.acceleration.z);
        hidStopSixAxisSensor(s_handle);
        startSensor();
        s_sameSince = g_ticks;
        return;
    }
    float ax = st.acceleration.x, ay = st.acceleration.y, az = st.acceleration.z;
    float n = sqrtf(ax * ax + ay * ay + az * az);
    if (n < 0.2f) return;  // nothing yet (or falling)
    ax /= n, ay /= n, az /= n;
    float plane = sqrtf(ax * ax + ay * ay);
    if (plane < 0.35f) return;  // lying flat (or nearly): keep whatever it was

    int want = s_cand;
    if (fabsf(ay) > fabsf(ax) * 1.4f) {  // clearly landscape (between the two, it stays as it was)
        s_upTime = SDL_clamp(s_upTime + (ay > 0 ? g_dt : -g_dt), -600.0f, 600.0f);
        if ((int)s_upTime != s_savedUp) {  // (kept, a second at a time)
            s_savedUp = (int)s_upTime;
            cfgSetInt("display.up_time", (int)(s_upTime * 10));
        }
        bool sure = fabsf(s_upTime) > 4;
        want = !sure || (ay > 0) == (s_upTime > 0) ? 0 : 180;
    } else if (fabsf(ax) > fabsf(ay) * 1.4f) {
        want = ax < 0 ? 90 : 270;
    }
    if (want != s_cand) {
        s_cand = want;
        s_candAt = g_ticks;
    }
    // held there for a moment, not just passing through
    if (s_cand != s_rot && g_ticks - s_candAt > 350) {
        s_rot = s_cand;
        appLog("orientation: %d (x %.2f, y %.2f, z %.2f)", s_rot, ax, ay, az);
    }
}
