#ifndef _CDE_H
#define _CDE_H

/*******************************************************************************
 * SYSTEM INCLUDES
 ******************************************************************************/

#include <stdbool.h>     // C99: bool, true, false

/*******************************************************************************
 * EXTERNALLY-DEFINED VARIABLES
 ******************************************************************************/

struct tcb;       // defs.h (strace module): process trace control block

/*******************************************************************************
 * PUBLIC VARIABLES
 ******************************************************************************/

extern char Cde_verbose_mode;    // print cde activity to stdout (-v option)
extern char Cde_exec_mode;       // false if auditing, true if running captured app
extern char Cde_app_dir[];       // abs path to cde app dir (contains cde-root)

/*******************************************************************************
 * PUBLIC MACROS / FUNCTIONS
 ******************************************************************************/

// like an assert except that it always fires
#define EXITIF(x) do { \
  if (x) { \
    fprintf(stderr, "Fatal error in %s [%s:%d]\n", __FUNCTION__, __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

// allocate heap memory for a tcb's cde fields
void alloc_tcb_cde_fields (struct tcb* tcp);
// free heap-allocated cde fields in a tcb
void free_tcb_cde_fields (struct tcb* tcp);
// use local network hostnames/etc during audit/exec
void use_local_network_settings (bool new_setting);

#endif // _CDE_H

