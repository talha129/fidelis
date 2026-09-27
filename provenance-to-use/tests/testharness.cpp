/*******************************************************************************
module:   testharness
author:   digimokan
date:     08 AUG 2017 (created)
purpose:  main on/off switch for using the doctest testing submodule
*******************************************************************************/

/*******************************************************************************
 * DOCTEST MAIN ON/OFF SWITCH
 ******************************************************************************/

// create command-line "doctest" executable that runs all test cases
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

/*******************************************************************************
 * DOCTEST SINGLE-HEADER LIB (MUST GO LAST)
 ******************************************************************************/

#include "doctest/doctest/doctest.h"

