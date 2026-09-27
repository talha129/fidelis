/*******************************************************************************
module:   strutils_test
author:   digimokan
date:     29 AUG 2017 (created)
purpose:  test cases for functions in strace-4.6/strutils.c
*******************************************************************************/

#include "doctest.h"
#include "strutils.h"

#include <cstring>      // ISOC: strlen()
#include <cstdbool>     // ISOC: bool, true, false

TEST_CASE("str_find_stripped_end") {

  SUBCASE("empty string") {
    char instr[] = "";
    char* stripend = str_find_stripped_end(instr);
    CHECK(*stripend == '\0');  // stripped end
  }

  SUBCASE("one-word string, no whitespace") {
    char instr[] = "foo";
    char* stripend = str_find_stripped_end(instr);
    CHECK(strncmp(instr, "foo", strlen(instr)) == 0);  // input not mutated
    CHECK(*stripend == '\0');  // stripped end
  }

  SUBCASE("one-word string, trailing whitespace") {
    char instr[] = "foo  ";
    char* stripend = str_find_stripped_end(instr);
    CHECK(strncmp(instr, "foo  ", strlen(instr)) == 0);  // input not mutated
    CHECK(strncmp(stripend, "  ", 2) == 0);  // stripped end
  }

  SUBCASE("two-word string, no whitespace") {
    char instr[] = "foo bar";
    char* stripend = str_find_stripped_end(instr);
    CHECK(strncmp(instr, "foo bar", strlen(instr)) == 0);  // input not mutated
    CHECK(*stripend == '\0');  // stripped end
  }

  SUBCASE("two-word string, trailing whitespace") {
    char instr[] = "foo bar  ";
    char* stripend = str_find_stripped_end(instr);
    CHECK(strncmp(instr, "foo bar  ", strlen(instr)) == 0);  // input not mutated
    CHECK(strncmp(stripend, "  ", 2) == 0);  // stripped end
  }

  SUBCASE("three-word string, no whitespace") {
    char instr[] = "foo bar zap";
    char* stripend = str_find_stripped_end(instr);
    CHECK(strncmp(instr, "foo bar zap", strlen(instr)) == 0);  // input not mutated
    CHECK(*stripend == '\0');  // stripped end
  }

  SUBCASE("three-word string, trailing whitespace") {
    char instr[] = "foo bar zap  ";
    char* stripend = str_find_stripped_end(instr);
    CHECK(strncmp(instr, "foo bar zap  ", strlen(instr)) == 0);  // input not mutated
    CHECK(strncmp(stripend, "  ", 2) == 0);  // stripped end
  }

  SUBCASE("multi-line string, no whitespace") {
    char instr[] = "foo bar zap\ngrok lom tux";
    char* stripend = str_find_stripped_end(instr);
    CHECK(strncmp(instr, "foo bar zap\ngrok lom tux", strlen(instr)) == 0);  // input not mutated
    CHECK(*stripend == '\0');  // stripped end
  }

  SUBCASE("multi-line string, trailing whitespace") {
    char instr[] = "foo bar zap\ngrok lom tux  ";
    char* stripend = str_find_stripped_end(instr);
    CHECK(strncmp(instr, "foo bar zap\ngrok lom tux  ", strlen(instr)) == 0);  // input not mutated
    CHECK(strncmp(stripend, "  ", 2) == 0);  // stripped end
  }

  SUBCASE("tabbed string, no whitespace") {
    char instr[] = "foo bar zap\tgrok lom tux";
    char* stripend = str_find_stripped_end(instr);
    CHECK(strncmp(instr, "foo bar zap\tgrok lom tux", strlen(instr)) == 0);  // input not mutated
    CHECK(*stripend == '\0');  // stripped end
  }

  SUBCASE("tabbed string, trailing whitespace") {
    char instr[] = "foo bar zap\tgrok lom tux  ";
    char* stripend = str_find_stripped_end(instr);
    CHECK(strncmp(instr, "foo bar zap\tgrok lom tux  ", strlen(instr)) == 0);  // input not mutated
    CHECK(strncmp(stripend, "  ", 2) == 0);  // stripped end
  }

}

TEST_CASE("str_rstrip") {

  SUBCASE("empty string") {
    char instr[] = "";
    char* rstripped = str_rstrip(instr);
    CHECK(instr == rstripped);  // return char* == input char*
    CHECK(*rstripped == '\0');  // stripped input
  }

  SUBCASE("one-word string, no whitespace") {
    char instr[] = "foo";
    size_t inlen = strlen(instr);
    char* rstripped = str_rstrip(instr);
    CHECK(instr == rstripped);  // return char* == input char*
    CHECK(strncmp(rstripped, "foo", inlen) == 0);  // stripped input
  }

  SUBCASE("one-word string, trailing whitespace") {
    char instr[] = "foo  ";
    size_t inlen = strlen(instr);
    char* rstripped = str_rstrip(instr);
    CHECK(instr == rstripped);  // return char* == input char*
    CHECK(strncmp(rstripped, "foo", inlen) == 0);  // stripped input
  }

  SUBCASE("three-word string, trailing whitespace") {
    char instr[] = "foo bar zap  ";
    size_t inlen = strlen(instr);
    char* rstripped = str_rstrip(instr);
    CHECK(instr == rstripped);  // return char* == input char*
    CHECK(strncmp(rstripped, "foo bar zap", inlen) == 0);  // stripped input
  }

  SUBCASE("multi-line string, trailing whitespace") {
    char instr[] = "foo bar zap\ngrok lom tux  ";
    size_t inlen = strlen(instr);
    char* rstripped = str_rstrip(instr);
    CHECK(instr == rstripped);  // return char* == input char*
    CHECK(strncmp(rstripped, "foo bar zap\ngrok lom tux", inlen) == 0);  // stripped input
  }

}

TEST_CASE("str_startswith") {

  SUBCASE("empty string starts with empty string") {
    char instr[] = "";
    bool swresult = str_startswith(instr, "");
    CHECK(swresult == true);
  }

  SUBCASE("empty string NOT start with some word") {
    char instr[] = "";
    char endstr[] = "foo";
    bool swresult = str_startswith(instr, endstr);
    CHECK(swresult == false);
  }

  SUBCASE("one-word string starts with same word") {
    char instr[] = "foo";
    bool swresult = str_startswith(instr, "foo");
    CHECK(swresult == true);
  }

  SUBCASE("one-word string NOT start with different word") {
    char instr[] = "foo";
    bool swresult = str_startswith(instr, "bar");
    CHECK(swresult == false);
  }

  SUBCASE("three-word string starts with some word") {
    char instr[] = "foo bar zap";
    bool swresult = str_startswith(instr, "foo");
    CHECK(swresult == true);
  }

  SUBCASE("three-word string NOT start with different word") {
    char instr[] = "foo bar zap";
    bool swresult = str_startswith(instr, "bar");
    CHECK(swresult == false);
  }

  SUBCASE("three-word string starts with empty string") {
    char instr[] = "foo bar zap";
    bool swresult = str_startswith(instr, "");
    CHECK(swresult == true);
  }

  SUBCASE("multi-line string starts with some word") {
    char instr[] = "foo bar zap\ngrok lom tux";
    bool swresult = str_startswith(instr, "foo");
    CHECK(swresult == true);
  }

  SUBCASE("multi-line string NOT start with different word") {
    char instr[] = "foo bar zap\ngrok lom tux";
    bool swresult = str_startswith(instr, "grok");
    CHECK(swresult == false);
  }

}

TEST_CASE("str_endswith") {

  SUBCASE("empty string ends with empty string") {
    char instr[] = "";
    bool ewresult = str_endswith(instr, "");
    CHECK(ewresult == true);
  }

  SUBCASE("empty string NOT end with some word") {
    char instr[] = "";
    char endstr[] = "foo";
    bool ewresult = str_endswith(instr, endstr);
    CHECK(ewresult == false);
  }

  SUBCASE("one-word string ends with same word") {
    char instr[] = "foo";
    bool ewresult = str_endswith(instr, "foo");
    CHECK(ewresult == true);
  }

  SUBCASE("one-word string NOT end with different word") {
    char instr[] = "foo";
    bool ewresult = str_endswith(instr, "bar");
    CHECK(ewresult == false);
  }

  SUBCASE("three-word string ends with some word") {
    char instr[] = "foo bar zap";
    bool ewresult = str_endswith(instr, "zap");
    CHECK(ewresult == true);
  }

  SUBCASE("three-word string NOT end with different word") {
    char instr[] = "foo bar zap";
    bool ewresult = str_endswith(instr, "foo");
    CHECK(ewresult == false);
  }

  SUBCASE("three-word string ends with empty string") {
    char instr[] = "foo bar zap";
    bool ewresult = str_endswith(instr, "");
    CHECK(ewresult == true);
  }

  SUBCASE("multi-line string ends with some word") {
    char instr[] = "foo bar zap\ngrok lom tux";
    bool ewresult = str_endswith(instr, "tux");
    CHECK(ewresult == true);
  }

  SUBCASE("multi-line string NOT end with different word") {
    char instr[] = "foo bar zap\ngrok lom tux";
    bool ewresult = str_endswith(instr, "zap");
    CHECK(ewresult == false);
  }

}

