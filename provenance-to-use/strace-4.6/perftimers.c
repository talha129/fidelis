/*******************************************************************************
module:   perftimers
author:   digimokan
date:     14 JUL 2017 (created)
purpose:  start, stop, and output specific pre-configured performance timers
timers:   AUDIT_FILE_COPYING (track total time spent copying files during audit)
*******************************************************************************/

/*******************************************************************************
 * SYSTEM INCLUDES
 ******************************************************************************/

#include <stdbool.h>        // ISOC: for bool data type
#include <time.h>           // P2001: for struct timespec, clock_gettime

/*******************************************************************************
 * USER INCLUDES
 ******************************************************************************/

#include "perftimers.h"

/*******************************************************************************
 * PRIVATE TYPES / CONSTANTS / VARIABLES
 ******************************************************************************/

#define NSEC_PER_SEC 1000000000     // nanosec per sec: for timespec arithmetic
#define NO_TIMERS 0                 // if no perf timers are enabled/started

static int timers_status = NO_TIMERS;   // bit flags track which timers enabled/disabled
static int timers_running = NO_TIMERS;  // bit flags track which timers started/stopped
static struct timespec start_times[NUM_TIMERS]; // save timer start times
static struct timespec total_times[NUM_TIMERS]; // save timer cumulative start-to-stop times

/*******************************************************************************
 * PRIVATE MACROS / FUNCTIONS
 ******************************************************************************/

// convert a PerfTimer enum into an array index based on its bit pos
static inline int get_index (const PerfTimer pt) {
  int bitpos = 1;   // start from right and shift this bit to left
  int index = 0;    // use to find index of bit set in pt (0 is rightmost index)

  // iterate until we find the '1' bit set in pt
  while (! (bitpos & pt)) {
    bitpos <<= 1;
    index++;
  }

  return index;
}

// return true if specific perf timer bit flag is "enabled"
static inline bool is_enabled (const PerfTimer pt) {
  return ( (bool) (timers_status & pt) );
}

// set specific perf timer "enabled" bit flag
static inline void set_enabled (const PerfTimer pt) {
  timers_status |= pt;
}

// unset specific perf timer "enabled" bit flag
static inline void set_disabled (const PerfTimer pt) {
  timers_status &= (~pt);
}

// return true if specific perf timer bit flag is "started"
static inline bool is_started (const PerfTimer pt) {
  return ( (bool) (timers_running & pt) );
}

// set specific perf timer "started" bit flag
static inline void set_started (const PerfTimer pt) {
  timers_running |= pt;
}

// unset specific perf timer "started" bit flag
static inline void set_stopped (const PerfTimer pt) {
  timers_running &= (~pt);
}

// subtract two timespecs: tstotal = tsa - tsb
static inline void sub_ts (struct timespec* const tstotal, const struct timespec* const tsa, const struct timespec* const tsb) {
  tstotal->tv_sec = tsa->tv_sec - tsb->tv_sec;
  tstotal->tv_nsec = tsa->tv_nsec - tsb->tv_nsec;
  if (tstotal->tv_nsec < 0) {
    tstotal->tv_sec--;
    tstotal->tv_nsec += NSEC_PER_SEC;
  }
}

// add two timespecs: tstotal = tsa + tsb
static inline void add_ts (struct timespec* const tstotal, const struct timespec* const tsa, const struct timespec* const tsb) {
  tstotal->tv_sec = tsa->tv_sec + tsb->tv_sec;
  tstotal->tv_nsec = tsa->tv_nsec + tsb->tv_nsec;
  if (tstotal->tv_nsec > NSEC_PER_SEC) {
    tstotal->tv_sec++;
    tstotal->tv_nsec -= NSEC_PER_SEC;
  }
}

// enable or disable specific perf timer and return success/error of the action
static inline TimerAction set_timer (const PerfTimer pt, const TimerStatus stat_req) {
  const bool pt_enabled = is_enabled(pt);
  TimerAction act = ERR_UNKNOWN_ERROR;

  // trying to enable timer that's already enabled: return err
  if ( (stat_req == ENABLED) && pt_enabled ) {
    act = ERR_TIMER_ALREADY_ENABLED;
  // trying to enable timer that's currently disabled: enable it
  } else if ( (stat_req == ENABLED) && (!pt_enabled) ) {
    const int ptindex = get_index(pt);
    total_times[ptindex].tv_sec = 0;
    total_times[ptindex].tv_nsec = 0;
    set_enabled(pt);
    act = SUCCESS_TIMER_ENABLED;
  // trying to disable timer that's currently enabled: disable it
  } else if ( ((stat_req == DISABLED)) && pt_enabled ) {
    set_disabled(pt);
    act = SUCCESS_TIMER_DISABLED;
  // trying to disable timer that's already disabled: return err
  } else if ( ((stat_req == DISABLED)) && (!pt_enabled) ) {
    act = ERR_TIMER_ALREADY_DISABLED;
  }

  return act;
}

// start specific perf timer and return success/error of the action
static inline TimerAction start_timer (const PerfTimer pt) {
  TimerAction act = ERR_UNKNOWN_ERROR;

  // trying to start timer that's not yet enabled: return err
  if (!is_enabled(pt)) {
    act = ERR_TIMER_NOT_ENABLED;
  // trying to start timer that's already started: return err
  } else if (is_started(pt)) {
    act = ERR_TIMER_ALREADY_STARTED;
  // timer is enabled but not currently running: start it
  } else {
    const int ptindex = get_index(pt);
    clock_gettime(CLOCK_MONOTONIC, &start_times[ptindex]);
    set_started(pt);
    act = SUCCESS_TIMER_STARTED;
  }

  return act;
}

// stop specific perf timer and return success/error of the action
static inline TimerAction stop_timer (const PerfTimer pt) {
  TimerAction act = ERR_UNKNOWN_ERROR;
  struct timespec tsstop;
  struct timespec tssubtotal;

  // trying to stop timer that's not yet enabled: return err
  if (!is_enabled(pt)) {
    act = ERR_TIMER_NOT_ENABLED;
  // trying to stop timer that's not yet started: return err
  } else if (!is_started(pt)) {
    act = ERR_TIMER_ALREADY_STOPPED;
  // timer is enabled and running: stop it and accumulate the run time
  } else {
    clock_gettime(CLOCK_MONOTONIC, &tsstop);
    const int ptindex = get_index(pt);
    sub_ts(&tssubtotal, &tsstop, &start_times[ptindex]);
    add_ts(&total_times[ptindex], &total_times[ptindex], &tssubtotal);
    set_stopped(pt);
    act = SUCCESS_TIMER_STOPPED;
  }

  return act;
}

// get total accum time of specific perf timer and return success/error of the action
static inline TimerAction get_total_time (const PerfTimer pt, double* total_time) {
  TimerAction act = ERR_UNKNOWN_ERROR;

  // trying to get total time but timer not yet enabled: return err
  if (!is_enabled(pt)) {
    act = ERR_TIMER_NOT_ENABLED;
  // trying to get total time but timer is running: return err
  } else if (is_started(pt)) {
    act = ERR_TIMER_RUNNING;
  // timer is enabled and stopped: return the total accumulated run time
  } else {
    const int ptindex = get_index(pt);
    *total_time = (double)total_times[ptindex].tv_sec +
                  ( (double)total_times[ptindex].tv_nsec / NSEC_PER_SEC );
    act = SUCCESS_TIMER_TOTAL_RETURNED;
  }

  return act;
}

/*******************************************************************************
 * PUBLIC FUNCTIONS
 ******************************************************************************/

// enable or disable specific enabled perf timer and return success/error of the action
// NOTE successful enable will zero out a timer's accumulated time
inline TimerAction set_perf_timer (PerfTimer pt, TimerStatus stat_req) {
  return set_timer(pt, stat_req);
}

// start specific enabled perf timer and return success/error of the action
inline TimerAction start_perf_timer (PerfTimer pt) {
  return start_timer(pt);
}

// stop specific enabled perf timer and return success/error of the action
inline TimerAction stop_perf_timer (PerfTimer pt) {
  return stop_timer(pt);
}

// get total accum time of specific enabled perf timer and return success/error of the action
inline TimerAction get_total_perf_time (PerfTimer pt, double* total_time) {
  return get_total_time(pt, total_time);
}

