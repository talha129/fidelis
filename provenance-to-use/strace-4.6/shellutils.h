/*******************************************************************************
module:   shellutils
author:   digimokan
date:     02 NOV 2017 (created)
purpose:  various shell utility functions
*******************************************************************************/

#ifndef SHELLUTILS_H
#define SHELLUTILS_H 1

// allow this header to be included from c++ source file
#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * PUBLIC INTERFACE
 ******************************************************************************/

// return a shell-safe str of input argstr, quoting it as necessary
char* malloc_quoted_arg_str (char* argstr);

// allow this header to be included from c++ source file
#ifdef __cplusplus
}
#endif

#endif // SHELLUTILS_H

