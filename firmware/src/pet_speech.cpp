#include "pet_speech.h"
#include "pet.h"
#include <stddef.h>

// ============================================================
// Line tables — 5..8 lines per category
// ============================================================

static const char* lines_greeting[] = {
    "oh, you're here",
    "hey there",
    "finally",
    "good to see you",
    "ready when you are",
    "oh! hi",
};

// NOTE: time-of-day categories use the UTC wall-clock. Hour boundaries will
// feel shifted for users outside UTC until pet_set_tz_offset() is wired up
// from the daemon payload. The logic is correct; only the midnight reference
// point is UTC rather than local.
static const char* lines_time_night[] = {
    "up late?",
    "the screen is so bright",
    "you should sleep",
    "another sprint?",
    "shhh, others are dreaming",
    "late-night commits hit different",
    "even I get tired",
};

static const char* lines_time_morning[] = {
    "good morning",
    "fresh code today?",
    "ready when you are",
    "smell that? coffee.",
    "let's go",
    "morning! got a plan?",
    "new day, new diff",
};

static const char* lines_time_afternoon[] = {
    "afternoon already",
    "how's the progress?",
    "got momentum?",
    "don't forget to stretch",
    "the build is waiting",
    "half the day done",
};

static const char* lines_time_evening[] = {
    "wrapping up?",
    "almost time to ship",
    "good work today",
    "commit before you forget",
    "evening logs look clean",
    "you earned a break",
    "one more?",
};

static const char* lines_high_xp_day[] = {
    "look at that XP!",
    "we're on a roll",
    "you've been busy",
    "more XP than usual",
    "productive session",
    "the diff is thicc",
    "I felt that commit",
    "leveling up soon?",
};

static const char* lines_low_stat[] = {
    "hey, I could use some care",
    "a little hungry here",
    "feeling a bit neglected",
    "my stats are slipping",
    "come on, help me out",
    "things aren't great...",
};

static const char* lines_hungry[] = {
    "I'm starving over here",
    "feed me?",
    "satiety is critical",
    "can't focus when hungry",
    "food = XP multiplier",
    "...stomach growling",
    "is anyone going to feed me",
};

static const char* lines_happy[] = {
    "I feel amazing",
    "this is the best",
    "all green, all day",
    "peak pet performance",
    "could level up any moment",
    "everything is good",
    "maximum vibes",
    "you're doing great",
};

static const char* lines_neglected[] = {
    "oh wow, you came back",
    "I missed you",
    "things got rough without you",
    "glad you're back",
    "don't disappear again",
    "you're forgiven... maybe",
};

static const char* lines_vacation[] = {
    "enjoy the break",
    "no depleting while you rest",
    "come back refreshed",
    "on holiday mode",
    "taking it easy",
    "vacation time!",
    "rest up, I'll be here",
    "see you on the other side",
};

static const char* lines_idle[] = {
    "just vibing",
    "waiting for commits",
    "all quiet on the main branch",
    "git status: peaceful",
    "bored? same.",
    "make something",
    "idle and watching",
    "no PRs? no problem",
};

// ============================================================
// Dispatch table — must stay in sync with speech_category_t enum order
// ============================================================

struct SpeechTable {
    const char** lines;
    uint8_t      count;
};

static const SpeechTable speech_tables[SPEECH__COUNT] = {
    { nullptr,                 0 },   // SPEECH_NONE       (0)
    { lines_greeting,          6 },   // SPEECH_GREETING   (1)
    { lines_time_night,        7 },   // SPEECH_TIME_NIGHT (2)
    { lines_time_morning,      7 },   // SPEECH_TIME_MORNING (3)
    { lines_time_afternoon,    6 },   // SPEECH_TIME_AFTERNOON (4)
    { lines_time_evening,      7 },   // SPEECH_TIME_EVENING (5)
    { lines_high_xp_day,       8 },   // SPEECH_HIGH_XP_DAY (6)
    { lines_low_stat,          6 },   // SPEECH_LOW_STAT (7)
    { lines_hungry,            7 },   // SPEECH_HUNGRY (8)
    { lines_happy,             8 },   // SPEECH_HAPPY (9)
    { lines_neglected,         6 },   // SPEECH_NEGLECTED (10)
    { lines_vacation,          8 },   // SPEECH_VACATION (11)
    { lines_idle,              8 },   // SPEECH_IDLE (12)
};

// ============================================================
// Public API
// ============================================================

uint8_t pet_speech_count(speech_category_t cat) {
    if (cat <= SPEECH_NONE || cat >= SPEECH__COUNT) return 0;
    return speech_tables[cat].count;
}

const char* pet_speech_pick(speech_category_t cat, uint32_t salt) {
    if (cat <= SPEECH_NONE || cat >= SPEECH__COUNT) return "";
    const SpeechTable& t = speech_tables[cat];
    if (!t.lines || t.count == 0) return "";
    return t.lines[salt % t.count];
}

// Priority-ordered category selection. First match wins.
speech_category_t pet_speech_current_category(void) {
    const Pet* p = pet_current();
    if (!p) return SPEECH_IDLE;

    // 1. Vacation active — everything else is paused anyway
    if (pet_is_vacation_active()) return SPEECH_VACATION;

    // 2. Satiety critically low (more specific than the generic LOW_STAT line)
    if (p->satiety < SAD_STAT_THRESHOLD) return SPEECH_HUNGRY;

    // 3. Any care stat below SAD_STAT_THRESHOLD
    if (p->spirit < SAD_STAT_THRESHOLD || p->bond < SAD_STAT_THRESHOLD) {
        return SPEECH_LOW_STAT;
    }

    // 4. Neglected: all stats are comfortably above 50 AND the pet hasn't
    //    earned XP in the last NEGLECT_ABSENCE_SEC (24 h). Surfaces the
    //    "welcome back after long absence" lines when the user comes back
    //    after at least a day away (P1-20).
    //
    //    Previously this checked `last_event_ts == born_ts`, which permanently
    //    disabled SPEECH_NEGLECTED after the first XP event for the pet's
    //    lifetime — making the 8 `lines_neglected` strings dead code for any
    //    returning user. Now uses pet_now() (exported from pet.h:116, was
    //    incorrectly described as "not exported" in the old comment) to do
    //    a true absence-duration check.
    //
    //    For brand-new pets that never had an XP event yet, require the pet
    //    to be at least 1 hour old so a fresh hatch doesn't fire NEGLECTED.
    //    NEGLECT_MIN_PET_AGE_SEC and NEGLECT_ABSENCE_SEC come from
    //    pet_balance_generated.h (via pet.h).
    if (p->satiety >= 50 && p->spirit >= 50 && p->bond >= 50) {
        uint32_t now = pet_now();
        uint32_t age = (now > p->born_ts) ? (now - p->born_ts) : 0;
        uint32_t absence = (now > p->last_event_ts) ? (now - p->last_event_ts) : 0;
        if (age >= NEGLECT_MIN_PET_AGE_SEC && absence >= NEGLECT_ABSENCE_SEC) {
            return SPEECH_NEGLECTED;
        }
    }

    // 5. Recent XP gain: last_event_ts is updated every XP grant in pet.cpp.
    //    last_event_ts and born_ts are pet_now()-based (monotonic uptime secs).
    //    If last_event_ts > born_ts the pet has earned XP at least once this
    //    uptime session — treat as active/recent. No wallclock comparison is
    //    possible because pet_now() is uptime-relative and pet_wallclock_now()
    //    is epoch-absolute; mixing the two units would produce garbage.
    if (p->last_event_ts > p->born_ts) {
        return SPEECH_HIGH_XP_DAY;
    }

    // 6. All stats >= 70 — happy pet
    if (p->satiety >= 70 && p->spirit >= 70 && p->bond >= 70) {
        return SPEECH_HAPPY;
    }

    // 7. Time-of-day (wall-clock, UTC until pet_set_tz_offset() is wired).
    //    pet_wallclock_now() returns 0 if daemon has never pushed a `ts` field.
    {
        uint32_t wc = pet_wallclock_now();
        if (wc != 0) {
            // g_tz_offset_sec is private to pet.cpp; using raw UTC for now.
            uint8_t hour = (uint8_t)((wc / 3600) % 24);
            if (hour >= 23 || hour < 5)  return SPEECH_TIME_NIGHT;
            if (hour < 12)               return SPEECH_TIME_MORNING;
            if (hour < 18)               return SPEECH_TIME_AFTERNOON;
            return SPEECH_TIME_EVENING;
        }
    }

    // 8. Catch-all
    return SPEECH_IDLE;
}
