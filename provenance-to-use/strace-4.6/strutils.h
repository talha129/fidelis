/*******************************************************************************
module:   strutils
author:   digimokan
date:     31 JUL 2017 (created)
purpose:  various c-string utility functions
*******************************************************************************/

#ifndef STRUTILS_H
#define STRUTILS_H 1

// allow this header to be included from c++ source file
#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * SYSTEM INCLUDES
 ******************************************************************************/

#include <stdbool.h>    // ISOC: bool

/*******************************************************************************
 * PUBLIC FUNCTIONS
 ******************************************************************************/

// return char* to pos just after last non-space char in str
char* str_find_stripped_end (char* str);

// remove trailing spaces from str, return the same str (now rstripped)
char* str_rstrip (char* str);

// return true if str starts with starting_substr
bool str_startswith (const char* str, const char* starting_substr);

// return true if str ends with ending_substr
bool str_endswith (const char* str, const char* ending_substr);

// allow this header to be included from c++ source file
#ifdef __cplusplus
}
#endif

#endif // STRUTILS_H

