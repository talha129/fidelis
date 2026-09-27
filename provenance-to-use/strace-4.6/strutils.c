/*******************************************************************************
module:   strutils
author:   digimokan
date:     31 JUL 2017 (created)
purpose:  various c-string utility functions
*******************************************************************************/

/*******************************************************************************
 * SYSTEM INCLUDES
 ******************************************************************************/

#include <stddef.h>     // ISOC: NULL
#include <stdbool.h>    // ISOC: bool, true, false
#include <string.h>     // ISOC: strlen()
#include <ctype.h>      // ISOC: isspace()

/*******************************************************************************
 * USER INCLUDES
 ******************************************************************************/

#include "strutils.h"

/*******************************************************************************
 * PRIVATE MACROS / FUNCTIONS
 ******************************************************************************/

/*******************************************************************************
 * PUBLIC FUNCTIONS
 ******************************************************************************/

// return char* to pos just after last non-space char in str
char* str_find_stripped_end (char* str) {
  char* stripped_end = str;

  for (char* p = str; *p != '\0'; p++) {
    if (!isspace(*p)) {
      stripped_end = p + 1;
    }
  }

  return stripped_end;
}

// remove trailing spaces from str, return the same str (now rstripped)
char* str_rstrip (char* str) {
  *(str_find_stripped_end(str)) = '\0';
  return str;
}

// return true if str starts with starting_substr
bool str_startswith (const char* str, const char* starting_substr) {
  return ( strncmp(str, starting_substr, strlen(starting_substr)) == 0 );
}

// return true if str ends with ending_substr
bool str_endswith (const char* str, const char* ending_substr) {
  size_t len_str = strlen(str);
  size_t len_es = strlen(ending_substr);

  // early out if str smaller than ending_substr
  if (len_str < len_es) {
    return false;
  }

  return ( strncmp(str + (len_str - len_es), ending_substr, len_es) == 0 );
}

