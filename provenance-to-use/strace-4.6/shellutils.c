/*******************************************************************************
module:   shellutils
author:   digimokan
date:     02 NOV 2017 (created)
purpose:  various shell utility functions
*******************************************************************************/

/*******************************************************************************
 * SYSTEM INCLUDES
 ******************************************************************************/

#include <string.h>     // ISOC: memcpy(), strchr(), strdup() [P2001]
#include <stdlib.h>     // ISOC: malloc()

/*******************************************************************************
 * USER INCLUDES
 ******************************************************************************/

#include "shellutils.h"

/*******************************************************************************
 * PUBLIC INTERFACE
 ******************************************************************************/

// return a shell-safe str of input argstr, quoting it as necessary
char* malloc_quoted_arg_str (char* argstr) {

  // if argstr contains only these safechars, no need to quote it
  const char* safechars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz1234567890@%_-+=:,./";
  size_t num_unsafe_chars = 0;
  size_t argstr_len = 0;

  // search for unsafe char in argstr
  for (char* ch = argstr; *ch != '\0'; ch++, argstr_len++) {
    if (strchr(safechars, *ch))
      continue;
    else
      num_unsafe_chars++;
  }

  // if all chars in argstr are safe, just return the original

  if (num_unsafe_chars == 0)
    return (strdup(argstr));

  // else surround argstr with single quotes, and replace any ' with '"'"'

  // quoted str to return, with space for surrounding/embedded single quotes
  char* quoted_str = (char*) malloc(argstr_len + 3 + (4 * num_unsafe_chars));
  char* qch = quoted_str;
  // append leading surrounding '
  *qch = '\'';
  qch++;
  // append all argstr, replacing any ' with '"'"'
  for (char* ach = argstr; *ach != '\0'; ach++) {
    if (*ach == '\'') {
      memcpy(qch, "'\"'\"'", 5);
      qch += 5;
    } else {
      *qch = *ach;
      qch++;
    }
  }
  // append ending surrounding ' and \0
  memcpy(qch, "'\0", 2);

  return (quoted_str);

}

