#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

enum session_state_t {
    SESSION_THINKING   = 0,
    SESSION_WAITING    = 1,
    SESSION_IDLE       = 2,
    SESSION_COMPACTING = 3,
};

struct SessionInfo {
    char id[9];              // 8 chars + null
    char name[24];           // up to 22 chars + null
    uint8_t ctx_pct;         // 0..100
    session_state_t state;
    uint16_t age_sec;        // seconds since last activity
};

#define SESSION_MAX 6

struct SessionList {
    uint8_t count;           // 0..SESSION_MAX
    SessionInfo items[SESSION_MAX];
    bool valid;              // false until first parse
};

const SessionList* current_sessions(void);
