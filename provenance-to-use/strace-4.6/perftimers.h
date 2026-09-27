/*******************************************************************************
module:   perftimers
author:   digimokan
date:     14 JUL 2017 (created)
purpose:  start, stop, and output specific pre-configured performance timers
timers:   AUDIT_FILE_COPYING (track total time spent copying files during audit)
*******************************************************************************/

#ifndef PERFTIMERS_H
#define PERFTIMERS_H 1

// allow this header to be included from c++ source file
#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * PUBLIC TYPES / CONSTANTS / VARIABLES
 ******************************************************************************/

// the current set of pre-defined perf timers
typedef enum {
  AUDIT_FILE_COPYING =  0x01,
} PerfTimer;
// current num timers defined in PerfTimer enum
#define NUM_TIMERS 1

// for enabling, disabling, or getting status of specific perf timer
typedef enum {
  DISABLED = 0,
  ENABLED =  1
} TimerStatus;

// for returning success of the enable/stop/start/get-total timer action
typedef enum {
  SUCCESS_TIMER_ENABLED,
  SUCCESS_TIMER_DISABLED,
  SUCCESS_TIMER_STARTED,
  SUCCESS_TIMER_STOPPED,
  SUCCESS_TIMER_TOTAL_RETURNED,
  ERR_TIMER_NOT_ENABLED,
  ERR_TIMER_ALREADY_ENABLED,
  ERR_TIMER_ALREADY_STARTED,
  ERR_TIMER_RUNNING,
  ERR_TIMER_ALREADY_STOPPED,
  ERR_TIMER_ALREADY_DISABLED,
  ERR_UNKNOWN_ERROR
} TimerAction;

/*******************************************************************************
 * PUBLIC MACROS / FUNCTIONS
 ******************************************************************************/

// enable or disable specific perf timer and return success/error of the action
// NOTE successful enable will zero out a timer's accumulated time
TimerAction set_perf_timer (PerfTimer pt, TimerStatus stat_req);

// start specific enabled perf timer and return success/error of the action
TimerAction start_perf_timer (PerfTimer pt);

// stop specific enabled perf timer and return success/error of the action
TimerAction stop_perf_timer (PerfTimer pt);

// get total accum time of specific enabled perf timer and return success/error of the action
TimerAction get_total_perf_time (PerfTimer pt, double* total_time);

// allow this header to be included from c++ source file
#ifdef __cplusplus
}
#endif

#endif // PERFTIMERS_H

