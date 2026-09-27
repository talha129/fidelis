/*******************************************************************************
module:   shellutils_test
author:   digimokan
date:     02 NOV 2017 (created)
purpose:  test cases for functions in strace-4.6/shellutils.c
*******************************************************************************/

#include "doctest.h"
#include "shellutils.h"

#include <cstring>      // ISOC: strlen(), strncmp()
#include <cstdlib>      // ISOC: free()

TEST_CASE("malloc_quoted_arg_str") {

  char* quoted_str = NULL;

  SUBCASE("argstr is empty string") {
    char argstr[] = "";
    quoted_str = malloc_quoted_arg_str(argstr);
    REQUIRE(quoted_str);
    CHECK(strlen(quoted_str) == 0);
  }

  SUBCASE("argstr contains one of each safe char") {
    char argstr[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz1234567890@%_-+=:,./";
    quoted_str = malloc_quoted_arg_str(argstr);
    REQUIRE(quoted_str);
    CHECK(strncmp(quoted_str, argstr, strlen(argstr)) == 0);
    CHECK(strlen(quoted_str) == strlen(argstr));
  }

  SUBCASE("argstr contains 3 unsafe chars in middle") {
    char argstr[] = "ABC$DEF$GHI$JKL";
    quoted_str = malloc_quoted_arg_str(argstr);
    char expected_qstr[] = "'ABC$DEF$GHI$JKL'";
    REQUIRE(quoted_str);
    CHECK(strncmp(quoted_str, expected_qstr, strlen(expected_qstr)) == 0);
    CHECK(strlen(quoted_str) == strlen(expected_qstr));
  }

  SUBCASE("argstr contains 3 unsafe chars at start, middle, and end") {
    char argstr[] = "$ABC$DEF$";
    quoted_str = malloc_quoted_arg_str(argstr);
    char expected_qstr[] = "'$ABC$DEF$'";
    REQUIRE(quoted_str);
    CHECK(strncmp(quoted_str, expected_qstr, strlen(expected_qstr)) == 0);
    CHECK(strlen(quoted_str) == strlen(expected_qstr));
  }

  SUBCASE("argstr contains 1 unsafe single-quote char in middle") {
    char argstr[] = "ABC'DEF";
    quoted_str = malloc_quoted_arg_str(argstr);
    char expected_qstr[] = "'ABC'\"'\"'DEF'";
    REQUIRE(quoted_str);
    CHECK(strncmp(quoted_str, expected_qstr, strlen(expected_qstr)) == 0);
    CHECK(strlen(quoted_str) == strlen(expected_qstr));
  }

  SUBCASE("argstr contains 2 unsafe single-quote chars in middle") {
    char argstr[] = "ABC'DEF'GHI";
    quoted_str = malloc_quoted_arg_str(argstr);
    char expected_qstr[] = "'ABC'\"'\"'DEF'\"'\"'GHI'";
    REQUIRE(quoted_str);
    CHECK(strncmp(quoted_str, expected_qstr, strlen(expected_qstr)) == 0);
    CHECK(strlen(quoted_str) == strlen(expected_qstr));
  }

  SUBCASE("argstr contains 3 unsafe single-quote chars in middle") {
    char argstr[] = "ABC'DEF'GHI'JKL";
    quoted_str = malloc_quoted_arg_str(argstr);
    char expected_qstr[] = "'ABC'\"'\"'DEF'\"'\"'GHI'\"'\"'JKL'";
    REQUIRE(quoted_str);
    CHECK(strncmp(quoted_str, expected_qstr, strlen(expected_qstr)) == 0);
    CHECK(strlen(quoted_str) == strlen(expected_qstr));
  }

  SUBCASE("argstr contains 3 unsafe single-quote chars at start, middle, end") {
    char argstr[] = "'ABC'DEF'";
    quoted_str = malloc_quoted_arg_str(argstr);
    char expected_qstr[] = "''\"'\"'ABC'\"'\"'DEF'\"'\"''";
    REQUIRE(quoted_str);
    CHECK(strncmp(quoted_str, expected_qstr, strlen(expected_qstr)) == 0);
    CHECK(strlen(quoted_str) == strlen(expected_qstr));
  }

  SUBCASE("argstr contains 6 unsafe chars, unsafe single-quote chars") {
    char argstr[] = "'AB$C'D$E$F'";
    quoted_str = malloc_quoted_arg_str(argstr);
    char expected_qstr[] = "''\"'\"'AB$C'\"'\"'D$E$F'\"'\"''";
    REQUIRE(quoted_str);
    CHECK(strncmp(quoted_str, expected_qstr, strlen(expected_qstr)) == 0);
    CHECK(strlen(quoted_str) == strlen(expected_qstr));
  }

  free(quoted_str);

}

