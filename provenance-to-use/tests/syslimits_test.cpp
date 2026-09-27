/*******************************************************************************
module:   syslimits_test
author:   digimokan
date:     08 AUG 2017 (created)
purpose:  test cases for functions in strace-4.6/syslimits.c
*******************************************************************************/

#include "doctest.h"
#include "syslimits.h"

#include <climits>    // ISOC: OPEN_MAX (possibly)
#include <cerrno>     // ISOC: errno()
#include <unistd.h>   // P2001: _SC_OPEN_MAX, sysconf()

TEST_CASE("max_open_files") {

  const long mof = max_open_files();            // return -1 or mof
  const long max_open_fallback = 256;           // max_open_files() fallback
  errno = 0;                                    // set by sysconf call if indet or err
  const long sysconf_result = sysconf(_SC_OPEN_MAX);  // returns mof, indeterminate, or err

#ifdef OPEN_MAX

  SUBCASE("OPEN_MAX defined") {
    CHECK(mof == OPEN_MAX);
  }

#else

  SUBCASE("OPEN_MAX not defined, sysconf returns indeterminate for mof") {
    if ( (sysconf_result < 0) && (errno == 0) ) {
      CHECK(mof == max_open_fallback);
    }
  }

  SUBCASE("OPEN_MAX not defined, sysconf returns error") {
    if ( (sysconf_result < 0) && (errno != 0) ) {
      CHECK(mof == -1);
    }
  }

  SUBCASE("OPEN_MAX not defined, sysconf() returns >= 0") {
    if (sysconf_result >= 0) {
      CHECK(mof >= 0);
    }
  }

#endif  // OPEN_MAX

}

