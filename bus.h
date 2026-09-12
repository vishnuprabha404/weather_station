#pragma once
#include <stddef.h>
#include "models.h"
#include "config.h" // for BUS_STOP_COUNT

// Fetches + merges GOVA Transit (Consat/tmix) real-time + static-schedule
// bus data for every stop configured in config.h's BUS_STOPS_CONFIG.
// `results` must have space for BUS_STOP_COUNT entries (results[i]
// corresponds to that same config's i-th stop). Returns true if AT LEAST
// ONE stop got any data at all (realtime or static) -- same "ANY success
// counts" contract as fetchAllWeather() in weather_station.ino, and for
// the same reason: a total failure of both sources for one stop shouldn't
// blank out a working result for another.
//
// Unlike weather's fetch functions, this does NOT take a "last known good"
// struct to merge into -- a bus ETA computed several minutes ago is simply
// wrong now, not "stale but usable" the way an old temperature reading is,
// so every call is a full, independent fetch+merge (see bus.cpp). What the
// caller does with a false return (e.g. keep showing the previous cycle's
// results on screen) is still a reasonable UI choice -- just not this
// function's job to make.
//
// See CLAUDE.md's "Real-Time Bus Data" section and clauderef.md for the
// full fetch/parse/merge strategy this ports from the user's existing
// Android widget / GNOME extension project (backend/gova_next_bus.py).
bool fetchAllBuses(BusStopResult* results);

// Formats an ETA the same way gova_next_bus.py's format_eta() does: "now"
// (ONLY for a live/GPS-backed prediction reading <=0 minutes -- a
// static-schedule guess reading 0 stays numeric, since it isn't a
// confirmed arrival), "N min" under an hour, "Hh Mm" (or just "Hh" exactly
// on the hour) at 60+ minutes.
void formatBusEta(int minutes, bool live, char* out, size_t outLen);

// Formats a BusArrival's epoch as 12-hour local clock time, e.g. "08:17 PM"
// -- zero-padded, matching this project's existing clock-formatting
// convention exactly (weather_station.ino's formatClockStrings() uses
// "%I:%M %p", not "%-I" -- the latter is a GNU strftime extension not
// confirmed present on this pinned toolchain, so this doesn't risk it
// either). Relies on the same TZ dependency as the rest of this project's
// time handling (configTzTime() at boot -- see weather_swob.cpp's comment
// on the same gap if WiFi isn't up yet at boot).
void formatBusClock12h(time_t epoch, char* out, size_t outLen);

// True if the two result sets would actually render differently on screen
// -- compares at DISPLAY precision (route, rounded minutes-to-arrival,
// live/scheduled status, delay, headsign) for each configured stop's
// shown arrivals, same "only redraw if it would actually look different"
// discipline weatherDisplayChanged() (weather.h) already uses. `count`
// must be config.h's BUS_STOP_COUNT for both arrays. Deliberately does NOT
// compare raw epoch/scheduledEpoch -- those tick every second internally
// but `minutes` (what's actually shown) only changes once a real minute
// has passed, which is the right granularity to redraw on.
bool busDisplayChanged(const BusStopResult* a, const BusStopResult* b, int count);
