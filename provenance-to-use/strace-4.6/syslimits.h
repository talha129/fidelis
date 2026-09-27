/*******************************************************************************
module:   syslimits
author:   digimokan
date:     21 JUL 2017 (created)
purpose:  obtain various limits of the current operating system
ref:      Advanced Programming In The UNIX Environment, 3rd Ed, Section 2.6
*******************************************************************************/

#ifndef SYSLIMITS_H
#define SYSLIMITS_H 1

// allow this header to be included from c++ source file
#ifdef __cplusplus
extern "C" {
#endif

// return the max num of open files - at any given time - on this system
long max_open_files (void);

// allow this header to be included from c++ source file
#ifdef __cplusplus
}
#endif

#endif // SYSLIMITS_H

