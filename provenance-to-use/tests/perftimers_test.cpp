/*******************************************************************************
module:   perftimers_test
author:   digimokan
date:     28 AUG 2017 (created)
purpose:  test cases for functions in strace-4.6/perftimers.c
*******************************************************************************/

#include "doctest.h"
#include "perftimers.h"

#include <ctime>      // P1993: struct timespec, nanosleep()

TEST_CASE("set_perf_timer") {

  TimerAction ta = set_perf_timer(AUDIT_FILE_COPYING, ENABLED);

  SUBCASE("enable perf timer") {
    CHECK(ta == SUCCESS_TIMER_ENABLED);
  }

  SUBCASE("enable disabled timer") {
    set_perf_timer(AUDIT_FILE_COPYING, DISABLED);
    CHECK(set_perf_timer(AUDIT_FILE_COPYING, ENABLED) == SUCCESS_TIMER_ENABLED);
  }

  SUBCASE("re-enable enabled perf timer") {
    CHECK(set_perf_timer(AUDIT_FILE_COPYING, ENABLED) == ERR_TIMER_ALREADY_ENABLED);
  }

  SUBCASE("enabled timer zeroes out total time") {
    double tt;
    TimerAction ta = get_total_perf_time(AUDIT_FILE_COPYING, &tt);
    CHECK(ta == SUCCESS_TIMER_TOTAL_RETURNED);
    CHECK(tt == doctest::Approx(0.0));
  }

  ta = set_perf_timer(AUDIT_FILE_COPYING, DISABLED);

  SUBCASE("disable perf timer") {
    CHECK(ta == SUCCESS_TIMER_DISABLED);
  }

  SUBCASE("re-disable disabled perf timer") {
    CHECK(set_perf_timer(AUDIT_FILE_COPYING, DISABLED) == ERR_TIMER_ALREADY_DISABLED);
  }

  SUBCASE("disable enabled timer") {
    set_perf_timer(AUDIT_FILE_COPYING, ENABLED);
    CHECK(set_perf_timer(AUDIT_FILE_COPYING, DISABLED) == SUCCESS_TIMER_DISABLED);
  }

}

TEST_CASE("start_perf_timer") {

  SUBCASE("start disabled perf timer") {
    set_perf_timer(AUDIT_FILE_COPYING, DISABLED);
    CHECK(start_perf_timer(AUDIT_FILE_COPYING) == ERR_TIMER_NOT_ENABLED);
  }

  SUBCASE("start enabled perf timer") {
    set_perf_timer(AUDIT_FILE_COPYING, ENABLED);
    CHECK(start_perf_timer(AUDIT_FILE_COPYING) == SUCCESS_TIMER_STARTED);
  }

  SUBCASE("start started perf timer") {
    CHECK(start_perf_timer(AUDIT_FILE_COPYING) == ERR_TIMER_ALREADY_STARTED);
    stop_perf_timer(AUDIT_FILE_COPYING);
  }

}

TEST_CASE("stop_perf_timer") {

  SUBCASE("stop disabled perf timer") {
    set_perf_timer(AUDIT_FILE_COPYING, DISABLED);
    CHECK(stop_perf_timer(AUDIT_FILE_COPYING) == ERR_TIMER_NOT_ENABLED);
  }

  SUBCASE("stop stopped perf timer") {
    set_perf_timer(AUDIT_FILE_COPYING, ENABLED);
    CHECK(stop_perf_timer(AUDIT_FILE_COPYING) == ERR_TIMER_ALREADY_STOPPED);
  }

  SUBCASE("stop started perf timer") {
    start_perf_timer(AUDIT_FILE_COPYING);
    CHECK(stop_perf_timer(AUDIT_FILE_COPYING) == SUCCESS_TIMER_STOPPED);
  }

}

TEST_CASE("get_total_perf_time") {

  SUBCASE("get total from disabled perf timer") {
    set_perf_timer(AUDIT_FILE_COPYING, DISABLED);
    double tt;
    TimerAction ta = get_total_perf_time(AUDIT_FILE_COPYING, &tt);
    CHECK(ta == ERR_TIMER_NOT_ENABLED);
  }

  SUBCASE("get total from running perf timer") {
    set_perf_timer(AUDIT_FILE_COPYING, ENABLED);
    start_perf_timer(AUDIT_FILE_COPYING);
    double tt;
    TimerAction ta = get_total_perf_time(AUDIT_FILE_COPYING, &tt);
    CHECK(ta == ERR_TIMER_RUNNING);
    stop_perf_timer(AUDIT_FILE_COPYING);
    set_perf_timer(AUDIT_FILE_COPYING, DISABLED);
  }

  struct timespec time_to_run;
  time_to_run.tv_sec = 0;
  time_to_run.tv_nsec = 10000000;  // 10 millisec

  SUBCASE("get total from perf timer that ran about 10 millisec") {

    set_perf_timer(AUDIT_FILE_COPYING, ENABLED);
    start_perf_timer(AUDIT_FILE_COPYING);
    nanosleep(&time_to_run, NULL);
    stop_perf_timer(AUDIT_FILE_COPYING);

    double tt;
    TimerAction ta = get_total_perf_time(AUDIT_FILE_COPYING, &tt);
    CHECK(ta == SUCCESS_TIMER_TOTAL_RETURNED);
    WARN_MESSAGE(tt == doctest::Approx(0.01).epsilon(0.001),  // allow 0.1% error (millisec accuracy)
                 "High system loads may interfere with timing for this test");
    set_perf_timer(AUDIT_FILE_COPYING, DISABLED);
  }

  SUBCASE("get total from perf timer that ran three times for total of about 30 millisec") {

    set_perf_timer(AUDIT_FILE_COPYING, ENABLED);
    double tt;
    TimerAction ta = get_total_perf_time(AUDIT_FILE_COPYING, &tt);
    CHECK(ta == SUCCESS_TIMER_TOTAL_RETURNED);
    CHECK(tt == doctest::Approx(0.0));

    for (int i = 0; i < 3; i++) {
      start_perf_timer(AUDIT_FILE_COPYING);
      nanosleep(&time_to_run, NULL);
      stop_perf_timer(AUDIT_FILE_COPYING);
    }

    ta = get_total_perf_time(AUDIT_FILE_COPYING, &tt);
    CHECK(ta == SUCCESS_TIMER_TOTAL_RETURNED);
    WARN_MESSAGE(tt == doctest::Approx(0.03).epsilon(0.001),  // allow 0.1% error (millisec accuracy)
                 "High system loads may interfere with timing for this test");
    set_perf_timer(AUDIT_FILE_COPYING, DISABLED);
  }

}

