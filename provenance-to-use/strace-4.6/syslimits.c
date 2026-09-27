/*******************************************************************************
module:   syslimits
author:   digimokan
date:     21 JUL 2017 (created)
purpose:  obtain various limits of the current operating system
ref:      Advanced Programming In The UNIX Environment, 3rd Ed, Section 2.6
*******************************************************************************/

/*******************************************************************************
 * SYSTEM INCLUDES
 ******************************************************************************/

#include <limits.h>   // ISOC: OPEN_MAX (possibly)
#include <unistd.h>   // P2001: _SC_OPEN_MAX, sysconf()
#include <errno.h>    // ISOC: errno()
#include <stdio.h>    // ISOC: perror()
#include <sys/resource.h>  // getrlimit, RLIMIT_NOFILE

/*******************************************************************************
 * USER INCLUDES
 ******************************************************************************/

#include "syslimits.h"

/*******************************************************************************
 * max_open_files()
 * return max num open files, at any given time, on this system (APUE 2.6)
 *******************************************************************************/

// if OPEN_MAX is defined, we're done.  else obtain the max programatically.
#ifdef OPEN_MAX
static long max_open = OPEN_MAX;
#else
static long max_open = 0;
#endif

// if unable to get the max by any means, use this number, which is *probably*
// greater than the undiscoverable actual max on the system (according to APUE)
static const long max_open_fallback = 256;

long max_open_files (void) {

  // OPEN_MAX not defined: obtain it once, programatically.
  if (max_open == 0) {

    // reset errno
    errno = 0;

    // Use hard limit from getrlimit so traced processes with higher soft limits
    // don't cause out-of-bounds writes into opened_file_paths[].
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_max != RLIM_INFINITY && rl.rlim_max > 0) {
      max_open = (long) rl.rlim_max;
    } else if ((max_open = sysconf(_SC_OPEN_MAX)) < 0) {
      // sysconf did not set errno value: limit is indeterminate
      if (errno == 0) {
        max_open = max_open_fallback;
      // sysconf set an errno value: problem executing sysconf
      } else {
        max_open = -1;
        perror("sysconf call in max_open_files()");
      }
    }

  }

  return max_open;
}

