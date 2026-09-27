/*******************************************************************************
 * SYSTEM INCLUDES
 ******************************************************************************/

#include <assert.h>      // C99/P2001: assert()
#include <ctype.h>       // C99/P2001: isspace()
#include <fcntl.h>       // P2001: O_RDONLY, O_WRONLY, O_RDWR
#include <pthread.h>     // P2001: PTHREAD_MUTEX_INITIALIZER, pthread_mutex_init/lock/unlock/create/destroy()
#include <pwd.h>         // P2001: getpwuid()
#include <sys/param.h>   // UNK: PATH_MAX
#include <strings.h>     // P2001: bzero()
#include <unistd.h>      // P2001: usleep()

/*******************************************************************************
 * USER INCLUDES
 ******************************************************************************/

#include "provenance.h"
#include "cde.h"
#include "okapi.h"      // canonicalize_path()
#include "const.h"
#include "syslimits.h"  // max_open_files()

/*******************************************************************************
 * EXTERNALLY-DEFINED FUNCTIONS
 ******************************************************************************/

extern void vbprintf (const char* fmt, ...); // cde.c
extern char* strcpy_from_child_or_null (struct tcb* tcp, long addr); // cde.c
extern void print_trace (void);              // strace's util.c
extern int string_quote (const char* instr, char* outstr, int len, int size); // strace's util.c

/*******************************************************************************
 * PUBLIC VARIABLES
 ******************************************************************************/

char Prov_prov_mode = 0;       // true if auditing (opposite of Cde_exec_mode)
char Prov_no_app_capture = 0;  // if true, run cde to collect prov but don't capture app

/*******************************************************************************
 * PRIVATE TYPES / CONSTANTS / VARIABLES
 ******************************************************************************/

// private types
typedef struct {
  pid_t pv[1000]; // array of pid's
  int pc;         // current num pid's in array
} PidList;

// private constants
#define ENV_LEN 16384     // max length of str to hold environ vars

// private variables
static FILE* prov_logfile = NULL; // provenance log file
static PidList pidlist;
static pthread_mutex_t mut_logfile = PTHREAD_MUTEX_INITIALIZER; // atomically update log file
static pthread_mutex_t mut_pidlist = PTHREAD_MUTEX_INITIALIZER; // atomically update pidlist

/*******************************************************************************
 * PRIVATE MACROS / FUNCTIONS
 ******************************************************************************/

#ifndef MAX
#  define MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef MIN
#  define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

// print something (? pidlist ?) to provlog
static void print_curr_prov (PidList* pidlist_p) {
  int i, curr_time;
  FILE *f;
  char buff[1024];
  long unsigned int rss;

  pthread_mutex_lock(&mut_pidlist);
  curr_time = (int)time(0);
  for (i = 0; i < pidlist_p->pc; i++) {
    sprintf(buff, "/proc/%d/stat", pidlist_p->pv[i]);
    f = fopen(buff, "r");
    if (f==NULL) { // remove this invalid pid
      fprintf(prov_logfile, "%d %u LEXIT\n", curr_time, pidlist_p->pv[i]); // lost_pid exit
      pidlist_p->pv[i] = pidlist_p->pv[pidlist_p->pc-1];
      pidlist_p->pc--;
      continue;
    }
    if (fgets(buff, 1024, f) == NULL)
      rss= 0;
    else
      // details of format: http://git.kernel.org/?p=linux/kernel/git/stable/linux-stable.git;a=blob_plain;f=fs/proc/array.c;hb=d1c3ed669a2d452cacfb48c2d171a1f364dae2ed
      sscanf(buff, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u "
          "%*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %*u %lu ", &rss);
    fclose(f);
    fprintf(prov_logfile, "%d %u MEM %lu\n", curr_time, pidlist_p->pv[i], rss);
    sprintf(buff, "/proc/%d/io", pidlist_p->pv[i]);
    f = fopen(buff, "r");
    if (f==NULL) continue; 

    fclose(f);
  }
  pthread_mutex_unlock(&mut_pidlist);
}

// add input pid to end of pidlist array
static void add_pid_prov (const pid_t pid) {
  pthread_mutex_lock(&mut_pidlist);
  pidlist.pv[pidlist.pc] = pid;
  pidlist.pc++;
  pthread_mutex_unlock(&mut_pidlist);
  print_curr_prov(&pidlist);
}

// remove input pid from pidlist array
static void rm_pid_prov (const pid_t pid) {
  int i=0;
  if (pidlist.pc<=0) return;
  print_curr_prov(&pidlist);
  pthread_mutex_lock(&mut_pidlist);
  while (pidlist.pv[i] != pid && i < pidlist.pc) i++;
  if (i < pidlist.pc) {
    pidlist.pv[i] = pidlist.pv[pidlist.pc-1];
    pidlist.pc--;
  }
  pthread_mutex_unlock(&mut_pidlist);
}

// log file read/write/rw to provlog
static void print_io_prov (struct tcb* tcp, const int path_index, const int action) {
  char *filename = strcpy_from_child_or_null(tcp, tcp->u_arg[path_index]);
  char *filename_abspath = canonicalize_path(filename, tcp->current_dir);
  assert(filename_abspath);

  fprintf(prov_logfile, "%d %u %s %s\n", (int)time(0), tcp->pid,
      (action == PRV_RDONLY ? "READ" : (
        action == PRV_WRONLY ? "WRITE" : (
        action == PRV_RDWR ? "READ-WRITE" : "UNKNOWNIO"))),
      filename_abspath);

  free(filename);
  free(filename_abspath);
}

/*
 * Print string specified by address `addr' and length `len'.
 * If `len' < 0, treat the string as a NUL-terminated string.
 * If string length exceeds `max_strlen', append `...' to the output.
 */
int
get_str_prov(char *dest, struct tcb *tcp, long addr, int len)
{
	static char *str = NULL;
	static char *outstr;
	int size;
  int len_written = 0;

	if (!addr) {
		len_written += sprintf(dest+len_written, "NULL");
		return len_written;
	}
	/* Allocate static buffers if they are not allocated yet. */
	if (!str)
		str = malloc(max_strlen + 1);
	if (!outstr)
		outstr = malloc(4 * max_strlen + sizeof "\"...\"");
	if (!str || !outstr) {
		fprintf(stderr, "out of memory\n");
		len_written += sprintf(dest+len_written, "%#lx", addr);
		return len_written;
	}

	if (len < 0) {
		/*
		 * Treat as a NUL-terminated string: fetch one byte more
		 * because string_quote() quotes one byte less.
		 */
		size = max_strlen + 1;
		str[max_strlen] = '\0';
		if (umovestr(tcp, addr, size, str) < 0) {
			len_written += sprintf(dest+len_written, "%#lx", addr);
			return len_written;
		}
	}
	else {
		size = MIN(len, max_strlen);
		if (umoven(tcp, addr, size, str) < 0) {
			len_written += sprintf(dest+len_written, "%#lx", addr);
			return len_written;
		}
	}

	if (string_quote(str, outstr, len, size) &&
	    (len < 0 || len > max_strlen))
		strcat(outstr, "...");

	len_written += sprintf(dest+len_written, "%s", outstr);
  return len_written;
}

void
print_arg_prov(char *argstr, struct tcb *tcp, long addr)
{
	union {
		unsigned int p32;
		unsigned long p64;
		char data[sizeof(long)];
	} cp;
	const char *sep;
	int n = 0;
  unsigned int len = 0;

  len += sprintf(argstr+len, "[");
	cp.p64 = 1;
	for (sep = ""; !abbrev(tcp) || n < max_strlen / 2; sep = ", ", ++n) {
		if (umoven(tcp, addr, personality_wordsize[current_personality],
			   cp.data) < 0) {
			len += sprintf(argstr+len, "%#lx\n", addr);
			return;
		}
		if (personality_wordsize[current_personality] == 4)
			cp.p64 = cp.p32;
		if (cp.p64 == 0)
			break;
		len += sprintf(argstr+len, "%s", sep);
		len += get_str_prov(argstr+len, tcp, cp.p64, -1);
		addr += personality_wordsize[current_personality];
	}
	if (cp.p64)
		len += sprintf(argstr+len, "%s...", sep);

	len += sprintf(argstr+len, "]");
}

void *capture_cont_prov(void* ptr) {
  PidList *pidlist_p = (PidList*) ptr;
  // Wait till we have the first pid, which should be the traced process.
  // Start recording memory footprint.
  // Ok to stop when there is no more pid, since by then,
  // the original tracded process should have stopped.
  while (pidlist_p->pc == 0) usleep(100000);
  while (pidlist_p->pc > 0) { // recording
    print_curr_prov(pidlist_p);
    sleep(1); // TODO: configurable
  } // done recording: pidlist.pc == 0
  pthread_mutex_destroy(&mut_pidlist);

	if (prov_logfile) {
    vbprintf("=== Close log file ===\n");
  	fclose(prov_logfile);
  }
  pthread_mutex_destroy(&mut_logfile);

  return NULL;
}

void rstrip(char *s) {
  size_t size;
  char *end;

  size = strlen(s);

  if (!size)
    return;

  end = s + size - 1;
  while (end >= s && isspace(*end))
    end--;
  *(end + 1) = '\0';

}

/*******************************************************************************
 * PUBLIC FUNCTIONS
 ******************************************************************************/

// initialize provlog file
void init_prov () {
  pthread_t ptid;
  char *env_prov_mode = getenv("IN_CDE_PROVENANCE_MODE");
  char path[PATH_MAX];
  int subns=1;

  if (env_prov_mode != NULL)
    Prov_prov_mode = (strcmp(env_prov_mode, "1") == 0) ? 1 : 0;
  else
    Prov_prov_mode = !Cde_exec_mode;
  if (Prov_prov_mode) {
    setenv("IN_CDE_PROVENANCE_MODE", "1", 1);
    pthread_mutex_init(&mut_logfile, NULL);
    // create NEW provenance log file
    bzero(path, sizeof(path));
    sprintf(path, "%s/provenance.%s.1.log", Cde_app_dir, CDE_ROOT_NAME);
    if (access(path, R_OK)==-1)
      prov_logfile = fopen(path, "w");
    else {
      // check through provenance.$subns.log to find a new file name
      do {
        subns++;
        bzero(path, sizeof(path));
        sprintf(path, "%s/provenance.%s.%d.log", Cde_app_dir, CDE_ROOT_NAME, subns);
      } while (access(path, R_OK)==0);
      fprintf(stderr, "Provenance log file: %s\n", path);
      prov_logfile = fopen(path, "w");
    }

    struct passwd *pw = getpwuid(getuid()); // don't free this pointer
    FILE *fp;
    char uname[PATH_MAX];
    fp = popen("uname -a", "r");
    if (fgets(uname, PATH_MAX, fp) == NULL)
      sprintf(uname, "(unknown architecture)");
    pclose(fp);
    rstrip(uname);
    char fullns[PATH_MAX];
    sprintf(fullns, "%s.%d", CDE_ROOT_NAME, subns);
    fprintf(prov_logfile, "# @agent: %s\n", pw == NULL ? "(noone)" : pw->pw_name);
    fprintf(prov_logfile, "# @machine: %s\n", uname);
    fprintf(prov_logfile, "# @namespace: %s\n", CDE_ROOT_NAME);
    fprintf(prov_logfile, "# @subns: %d\n", subns);
    fprintf(prov_logfile, "# @fullns: %s\n", fullns);
    fprintf(prov_logfile, "# @parentns: %s\n", getenv("CDE_PROV_NAMESPACE"));

    setenv("CDE_PROV_NAMESPACE", fullns, 1);

    pthread_mutex_init(&mut_pidlist, NULL);
    pidlist.pc = 0;
    pthread_create( &ptid, NULL, capture_cont_prov, &pidlist);
  }
}

// log beginning proc exec call to provlog if auditing, to stderr if verbose
void print_begin_execve_prov (struct tcb* tcp) {
  if (Prov_prov_mode) {
    char *opened_filename = strcpy_from_child_or_null(tcp, tcp->u_arg[0]);
    char *filename_abspath = canonicalize_path(opened_filename, tcp->current_dir);
    assert(filename_abspath);
    int parentPid = tcp->parent == NULL ? getpid() : tcp->parent->pid;
    char args[KEYLEN*10];
    print_arg_prov(args, tcp, tcp->u_arg[1]);

    fprintf(prov_logfile, "%d %d EXECVE %u %s %s %s\n", (int)time(0),
      parentPid, tcp->pid, filename_abspath, tcp->current_dir, args);

    if (Cde_verbose_mode) {
      vbprintf("[%d-prov] BEGIN %s '%s'\n", tcp->pid, "execve", opened_filename);
    }

    free(filename_abspath);
    free(opened_filename);
  }
}

// log ending proc exec call to provlog if auditing, to stderr if verbose
void print_end_execve_prov (struct tcb* tcp) {
  if (Prov_prov_mode) {
    int ppid = -1;
    if (tcp->parent) ppid = tcp->parent->pid;

    fprintf(prov_logfile, "%d %u EXECVE2 %d\n", (int)time(0), tcp->pid, ppid);
    add_pid_prov(tcp->pid);
    if (Cde_verbose_mode) {
      vbprintf("[%d-prov] BEGIN execve2\n", tcp->pid);
    }
  }
}

// log proc creation of new proc to provlog if auditing
void print_spawn_prov(struct tcb *tcp) {
  if (Prov_prov_mode) {
    fprintf(prov_logfile, "%d %u SPAWN %u\n", (int)time(0), tcp->parent->pid, tcp->pid);
  }
}

// log proc exit call (to terminate itself) to provlog if auditing
void print_exit_prov (struct tcb* tcp) {
  if (Prov_prov_mode) { // not handle exit by signal yet
    rm_pid_prov(tcp->pid);
    fprintf(prov_logfile, "%d %u EXIT\n", (int)time(0), tcp->pid);
  }
}

// log file open/openat to provlog if auditing, to stderr if verbose
void print_open_prov (struct tcb* tcp, const char* syscall_name) {

  // assume openAT uses the same current_dir with PWD (from CDE code)
  // open u_args:   [abs path] [r/w/rw] [permissions]
  //      u_rval:   fd if ok, -1 if error
  // openat u_args: [rel path] [fd for abs dir] [r/w/rw] [permissions]
  //      u_rval:   fd if ok, -1 if error

  // set path_index to 1 if open, 2 if openat, or return if bad syscall_name
  int path_index;
  if (strcmp(syscall_name, "sys_open") == 0) {
    path_index = 1;
  } else if (strcmp(syscall_name, "sys_openat") == 0) {
    path_index = 2;
  } else {
    return;
  }

  // get abs path used to open file
  char* filename = strcpy_from_child_or_null(tcp, tcp->u_arg[path_index-1]);
  char* filename_abspath = canonicalize_path(filename, tcp->current_dir);

  // log prov if in prov mode and successful open call (on valid file)
  if (Prov_prov_mode && (tcp->u_rval >= 0)) {

    // get r/w/rw mode bits
    unsigned char open_mode = (tcp->u_arg[path_index] & 3);

    // translate mode bits into mode enum
    int action;
    switch (open_mode) {
      case O_RDONLY: action = PRV_RDONLY; break;
      case O_WRONLY: action = PRV_WRONLY; break;
      case O_RDWR: action = PRV_RDWR; break;
      default: action = PRV_UNKNOWNIO; break;
    }

    // log the open call to prov log
    print_io_prov(tcp, path_index - 1, action);

    // store exact abs path used to open the file (guard against traced process
    // using fds beyond PTU's allocated opened_file_paths[] size)
    if (tcp->u_rval >= 0 && tcp->u_rval < max_open_files()) {
      freeifnn(tcp->opened_file_paths[tcp->u_rval]);
      tcp->opened_file_paths[tcp->u_rval] = strdup(filename_abspath);
    }
  }

  // log to stderr if verbose
  if (Cde_verbose_mode >= 1) {
    vbp(1, "%s: fd= %ld\n", filename_abspath, tcp->u_rval);
  }

  freeifnn(filename);
  freeifnn(filename_abspath);
}

// log file read to provlog if auditing, to stderr if verbose
void print_read_prov (struct tcb* tcp, const char* syscall_name, const int path_index) {
  if (Prov_prov_mode && tcp->u_rval >= 0) {
    print_io_prov(tcp, path_index, PRV_RDONLY);
  }
}

// log file write to provlog if auditing, to stderr if verbose
void print_write_prov (struct tcb* tcp, const char* syscall_name, const int path_index) {
  if (Prov_prov_mode && tcp->u_rval >= 0) {
    print_io_prov(tcp, path_index, PRV_WRONLY);
  }
}

// log file hardlink/symlink to provlog if auditing
void print_link_prov (struct tcb* tcp, const char* syscall_name,
                      const int realpath_index, const int linkpath_index) {
  if ( (Prov_prov_mode) && (tcp->u_rval == 0) ) {
      print_io_prov(tcp, realpath_index, PRV_RDONLY);
      print_io_prov(tcp, linkpath_index, PRV_WRONLY);
  }
}

// log file rename/move to provlog if auditing
void print_rename_prov (struct tcb* tcp, const int renameat) {
  if (Prov_prov_mode) {
    if (tcp->u_rval == 0) {
      // assume renameat uses the same current_dir with PWD (from CDE code)
      int origpath_index = renameat == 0 ? 0 : 1;
      int newpath_index = renameat == 0 ? 1 : 3;
      print_io_prov(tcp, origpath_index, PRV_RDWR);
      print_io_prov(tcp, newpath_index, PRV_WRONLY);
    }
  }
}

// log file close to provlog if auditing, to stderr if verbose
void print_close_prov (struct tcb* tcp) {
  // exit early if not in prov mode (i.e. not auditing)
  if (!Prov_prov_mode) {
    return;
  }

  // get close sys call's close fd from sys call's pcb
  const int closefd = tcp->u_arg[0];

  // will hold stored exact abs path used to open the file
  const char* openpath;

  // exit early if fd is out of range or was not opened by this proc
  if (closefd < 0 || closefd >= max_open_files() || tcp->opened_file_paths[closefd] == NULL) {
    return;
  }
  openpath = tcp->opened_file_paths[closefd];

  // log to provlog
  fprintf(prov_logfile, "%d %u %s %s\n", (int)time(0), tcp->pid, "CLOSE", openpath);

  // log to stderr if verbose
  if (Cde_verbose_mode) {
    vbprintf("[%d-prov] CLOSE %s\n", tcp->pid, openpath);
  }
}

