#ifndef _PROVENANCE_H
#define _PROVENANCE_H

/*******************************************************************************
 * EXTERNALLY-DEFINED VARIABLES
 ******************************************************************************/

struct tcb;       // defs.h (strace module): process trace control block

/*******************************************************************************
 * PUBLIC TYPES / CONSTANTS / VARIABLES
 ******************************************************************************/

extern char Prov_prov_mode;        // true if auditing (opposite of Cde_exec_mode)
extern char Prov_no_app_capture;   // if true, run cde to collect prov but don't capture app

/*******************************************************************************
 * PUBLIC MACROS / FUNCTIONS
 ******************************************************************************/

// initialize provlog file
void init_prov ();
// log proc exec call to provlog if auditing, to stderr if verbose
void print_begin_execve_prov (struct tcb* tcp);
// log ending proc exec call to provlog if auditing, to stderr if verbose
void print_end_execve_prov (struct tcb* tcp);
// log proc creation of new proc to provlog if auditing
void print_spawn_prov (struct tcb* tcp);
// log proc exit call (to terminate itself) to provlog if auditing
void print_exit_prov(struct tcb *tcp);
// log file open/openat to provlog if auditing, to stderr if verbose
void print_open_prov (struct tcb* tcp, const char* syscall_name);
// log file read to provlog if auditing, to stderr if verbose
void print_read_prov (struct tcb* tcp, const char* syscall_name, const int path_index);
// log file write to provlog if auditing, to stderr if verbose
void print_write_prov (struct tcb* tcp, const char* syscall_name, const int path_index);
// log file hardlink/symlink to provlog if auditing
void print_link_prov (struct tcb* tcp, const char* syscall_name, const int realpath_index, const int linkpath_index);
// log file rename/move to provlog if auditing
void print_rename_prov (struct tcb* tcp, const int renameat);
// log file close to provlog if auditing, to stderr if verbose
void print_close_prov (struct tcb* tcp);

#endif // _PROVENANCE_H

