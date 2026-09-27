# Provenance To Use

PTU is a Linux utility that captures a complete runtime environment,
dependencies, and program data - along with the associated provenance of each -
for any Linux application simply by studying a live sample of the application's
runtime behavior.  Subsequently, PTU can use the capture to reproduce and run
the whole application (or a specific subpart of it) on a bare Linux machine.

## Table of Contents

* [Terms And Definitions](#markdown-header-terms-and-definitions)
* [Installation](#markdown-header-installation)
    * [Fedora](#markdown-header-fedora)
        * [Fedora 25 Workstation](#markdown-header-fedora-25-workstation)
    * [Ubuntu](#markdown-header-ubuntu)
        * [Ubuntu Desktop 16-04](#markdown-header-ubuntu-desktop-16-04)
        * [Ubuntu Mate 16-10](#markdown-header-ubuntu-mate-16-10)
        * [Ubuntu Desktop 17-04](#markdown-header-ubuntu-desktop-17-04)
    * [CentOS](#markdown-header-centos)
        * [CentOS 7](#markdown-header-centos-7)
    * [Arch Linux](#markdown-header-arch-linux)
        * [Antergos](#markdown-header-antergos)
* [Usage](#markdown-header-usage)
    * [Capturing An Application](#markdown-header-capturing-an-application)
        * [Capture Commands](#markdown-header-capture-commands)
        * [Created Capture Files](#markdown-header-created-capture-files)
    * [Running A Captured Application](#markdown-header-running-a-captured-application)
        * [Running The Captured Application](#markdown-header-running-the-captured-application)
* [Architecture](#markdown-header-architecture)
    * [High Level Architecture](#markdown-header-high-level-architecture)
    * [Source Code Layout](#markdown-header-source-code-layout)
    * [Modules And Functions](#markdown-header-modules-and-functions)
* [Git Workflow](#markdown-header-git-workflow)
    * [Initial Setup](#markdown-header-initial-setup)
    * [Development Workflow](#markdown-header-development-workflow)
    * [Merging Pull Requests](#markdown-header-merging-pull-requests)
* [Testing](#markdown-header-testing)
    * [Testing Framework](#markdown-header-testing-framework)
    * [Installing Testing Framework](#markdown-header-installing-testing-framework)
    * [Updating Testing Framework](#markdown-header-updating-testing-framework)
    * [Building PTU With Test Harness](#markdown-header-building-ptu-with-test-harness)
    * [Writing Tests](#markdown-header-writing-tests)
    * [Running Tests](#markdown-header-running-tests)
* [Project Team](#markdown-header-project-team)
* [License](#markdown-header-license)

## Terms And Definitions

* __audit__: capture an application by running it with PTU
* __reference execution__: a single "run" of a captured application.  If the
  same application is captured multiple times, each run will produce a new
  reference execution.

## Installation

### Fedora

#### Fedora 25 Workstation

1. Install a Fedora 25 Workstation iso to a machine or VM with at least 20GB of
disk space and 4GB of RAM.

2. Install required PTU dependencies:

        $ dnf install git cmake make gcc

3. Download the PTU application via Bitbucket:

        $ git clone --recursive https://[your-bitbucket-username]@bitbucket.org/depauldbgroup/provenance-to-use.git

4. Change to the newly-cloned PTU directory:

        $ cd provenance-to-use

5. Make build directory, run cmake, and build ptu app (release version, with no
tests):

        $ ./run.sh -r

### Ubuntu

#### Ubuntu Desktop 16-04

1. Install an Ubuntu Desktop 16-04 iso to a machine or VM with at least 20GB of
disk space and 4GB of RAM.

2. Install required PTU dependencies:

        $ apt-get install git cmake make gcc

3. Download the PTU application via Bitbucket:

        $ git clone --recursive https://[your-bitbucket-username]@bitbucket.org/depauldbgroup/provenance-to-use.git

4. Change to the newly-cloned PTU directory:

        $ cd provenance-to-use

5. Make build directory, run cmake, and build ptu app (release version, with no
tests):

        $ ./run.sh -r

#### Ubuntu Mate 16-10

1. Install an Ubuntu Mate 16.10 iso to a machine or VM with at least 20GB of
disk space and 4GB of RAM.

2. Install required PTU dependencies:

        $ apt-get install git cmake make gcc

3. Download the PTU application via Bitbucket:

        $ git clone --recursive https://[your-bitbucket-username]@bitbucket.org/depauldbgroup/provenance-to-use.git

4. Change to the newly-cloned PTU directory:

        $ cd provenance-to-use

5. Make build directory, run cmake, and build ptu app (release version, with no
tests):

        $ ./run.sh -r

#### Ubuntu Desktop 17-04

1. Install an Ubuntu Desktop 17-04 iso to a machine or VM with at least 20GB of
disk space and 4GB of RAM.

2. Install required PTU dependencies:

        $ apt-get install git cmake make gcc

3. Download the PTU application via Bitbucket:

        $ git clone --recursive https://[your-bitbucket-username]@bitbucket.org/depauldbgroup/provenance-to-use.git

4. Change to the newly-cloned PTU directory:

        $ cd provenance-to-use

5. Make build directory, run cmake, and build ptu app (release version, with no
tests):

        $ ./run.sh -r

### CentOS

#### CentOS 7

1. Install a CentOS 7.0 iso to a machine or VM with at least 20GB of disk space
and 4GB of RAM.  Select the Gnome graphical environment option.  Deselect the
Security Policy option (i.e. set "Apply Security Policy" to off, disabling
SELinux).

2. Install the EPEL respository (which contains the gv package), then install
required PTU dependencies and create link to needed C++ library:

        $ yum install git cmake make gcc

3. Download the PTU application via Bitbucket:

        $ git clone --recursive https://[your-bitbucket-username]@bitbucket.org/depauldbgroup/provenance-to-use.git

4. Change to the newly-cloned PTU directory:

        $ cd provenance-to-use

5. Make build directory, run cmake, and build ptu app (release version, with no
tests):

        $ ./run.sh -r

### Arch Linux

#### Antergos

NOTE: Since Antergos is a rolling distribution based on Arch Linux, its
"version number" is best described as a given point in time; this setup was
tested with an up-to-date Antergos install on 06 AUG 2017.

1. Install an Antegros iso to a machine or VM with at least 20GB of disk space
and 4GB of RAM.  Select the KDE desktop environment option.

2. Install required PTU dependencies:

        $ pacman -S git cmake make gcc

3. Download the PTU application via Bitbucket:

        $ git clone --recursive https://[your-bitbucket-username]@bitbucket.org/depauldbgroup/provenance-to-use.git

4. Change to the newly-cloned PTU directory:

        $ cd provenance-to-use

5. Make build directory, run cmake, and build ptu app (release version, with no
tests):

        $ ./run.sh -r

## Usage

### Capturing An Application

#### Capture Commands

To capture an application (i.e. either an executable binary, or script file),
use  ptu to run the application:

        $ /path/to/provenance-to-use/ptu /path/to/binary-or-script [binary-or-script arguments]

For example:

        $ cd /home/user1
        $ /home/user1/provenance-to-use/ptu /usr/bin/mutt -R

The application will launch and run normally.  As it runs, PTU will capture and
package the applications environment, dependencies, and data.  When you quit the
application, the complete packaging will be copied to a new "ptu-package"
directory in the current working directory.

#### Created Capture Files

NOTE: this section may be of more use to PTU developers, and may not be
pertinent to users only interested in capturing and running applications.

After PTU is finished capturing an application, it will create a "ptu-package"
directory in the current working directory. The ptu-package directory will
contain the following important files and directories:

* `cde-root`: a directory containing a snapshot/sandbox of all files and
executables used by the app, stored in a directory structure identical to the
structure on the host machine.
* `[binary-or-script-name.cde]`: a special "shortcut" script that will run the
captured application from within the sandbox.  It will be located within the
`cde-root` dir in the same location that the above capture command was run from
(e.g. for the capture command example above, `mutt.cde` will be located at
`/home/user1/ptu-package/cde-root/home/user1/mutt.cde`).
* `cde-exec`: an executable program utilized by the shortcut script.  This
program redirects calls made by the captured application into the `cde-root`
sandbox.
* `cde.uname`: a text file containing details about the host machine's
architecture and operating system.
* `cde.full-environment.cde-root`: a text file containing all the environment
variables that existed when the capture command was run.
* `cde.options`: a text file containing user-configurable options that modify
the behavior of `cde-exec`.
* `cde.log`: a text file containing the commands used to execute the original
application capture.
* `provenance.cde-root.1.log`: a log file of all the processes, files, system
memory, and other resources accessed by the captured app while it was running.

### Running A Captured Application

#### Running The Captured Application

1. Change to the `ptu-package/cde-root` sandbox directory.

2. Change to the directory within `cde-root` that contains the shortcut script
file `[binary-or-script-name.cde]` (See
[Created Capture Files](#markdown-header-created-capture-files) above for
explanation).

3. Run the shortcut script file by invoking the same command (with the same
options) that was run to capture the application:

        $ /path/to/cde-root/path/to/binary-or-script.cde [binary-or-script arguments]

    For the above example, the following command would run the captured application:

        $ /home/user1/ptu-package/cde-root/home/user1/mutt.cde -R

## Architecture

NOTE: files and directories annotated with a `*` are fixed dependencies modified
by this project.  Files and directories NOT annotated with a `*` have been
created by this project.

### High Level Architecture

![Architecture](/readme/ptu-architecture.png)

### Source Code Layout

```
provenance-to-use/
├── build/              # Cmake build output
│   ├── /config.h       # Conditional defs using config.h.in & CMakeLists logic
├── strace-4-6*/        # Capture app, run app, track provenance
│   ├── /desc.c*        # System calls for file close, fd dup, and other misc
│   ├── /cde.c          # Audit app or run captured app
│   ├── /defs.h*        # Conditional defs/libs using config.h as input
│   ├── /file.c*        # System calls to trace file access
│   ├── /okapi.c        # Copy files/dirs/simlinks with structural fidelity
│   ├── /perftimers.c   # Optional performance timing of ptu code segments
│   ├── /process.c*     # System calls to trace process actions
│   ├── /provenance.c   # Record app prov info to text log and to database
│   ├── /strace.c*      # Main entry point: runs and traces app for audit/capture
│   ├── /syslimits.c    # Obtain OS maxes for num open files, command-line length, etc.
├── readelf-mini*/      # Read contents of files by file type
│   ├── /readelfmini.c* # Read contents of an ELF file
├── config.h.in         # Template to define defs based on CMakeLists.txt logic
├── CMakeLists.txt      # Cmake config file for building ptu project
├── run.sh              # Script to create build dir, run cmake, and build ptu
```

### Modules And Functions

```
├── provenance.c                  # Record app prov info to text log file
│   ├── init_prov()               # Initialize prov text log file
│   ├── print_begin_execve_prov() # Log process execve (starting info) sys call
│   ├── print_end_execve_prov()   # Log process execve (ending info) sys call
│   ├── print_spawn_prov()        # Log process creation of new process
│   ├── print_exit_prov()         # Log process exit sys call
│   ├── print_open_prov()         # Log file open/openat sys call
│   ├── print_read_prov()         # Log file read sys call
│   ├── print_write_prov()        # Log file write sys call
│   ├── print_link_prov()         # Log file hardlink/symlink creation sys call
│   ├── print_rename_prov()       # Log file rename/move sys call
│   ├── print_close_prov()        # Log file close sys call
├── desc.c*                       # System calls for file close, fd dup, and other misc
│   ├── sys_close()*              # Sys call: close file
├── defs.h*                       # Conditional defs/libs using config.h as input
│   ├── entering()*               # Return T if app just made syscall, F if kernel just processed syscall
├── file.c*                       # System calls to trace file access
│   ├── sys_open()*               # Sys call: open file
│   ├── sys_openat()*             # Sys call: open file relative to specified dir
├── readelf-mini.c*               # Read contents of an ELF file
│   ├── find_ELF_program_interpreter()* # find name of prog interp for ELF binary
├── strace.c*                     # Capture app, run app, track provenance
│   ├── main()*                   # Main entry point for ptu application
```

## Git Workflow

### Initial Setup

1. [BITBUCKET PAGE] Fork the project repo:

    * click "fork" from https://bitbucket.org/depauldbgroup/provenance-to-use

2. [LOCAL] Create local repo:

        $ git clone --recursive https://bitbucket.org/[your-bitbucket-username]/provenance-to-use.git

3. [LOCAL] Link upstream repo:

        $ git remote add upstream https://bitbucket.org/depauldbgroup/provenance-to-use

### Development Workflow

1. Find the issue you have been assigned, or assign a needed issue to yourself.

2. [LOCAL] Retrieve all changes from upstream.  Update local master branch and
sync it with your forked origin repo.  Create new local branches to track
upstream branches you want to follow locally:

        $ git fetch upstream
        $ git checkout master
        $ git merge upstream/master
        $ git push origin master
        $ git branch --track [branch-name] upstream/[branch-name]

3. [LOCAL] Create and switch to a feature/fix branch for your issue:

        $ git checkout master
        $ git checkout -b feat-issuename

4. [LOCAL] Work on your feature branch:

        $ [edit existing files / new files]
        $ git add [existing/new files]
        $ git commit

5. [LOCAL] Periodically merge in upstream changes into your feature branch.
When working on feature branch for long periods, this merging reduces the
confusion that may come with a single large merge in step 8:

        $ git fetch upstream
        $ git checkout master
        $ git merge upstream/master
        $ git push origin master
        $ git checkout feat-issuename
        $ git merge master

6. [LOCAL] Periodically push your feature branch to your forked origin repo
(to back it up), and also push your feature branch to upstream (so that others
may view and comment on your progress):

        $ git push origin feat-issuename
        $ git push upstream feat-issuename

7. [LOCAL] When complete with your feature branch, retrieve all changes from
upstream.  Update local master branch and sync it with your forked origin repo.
Create new local branches to track new upstream branches you want to follow
locally.  Update other existing local branches with their upstream counterparts:

        $ git fetch upstream
        $ git checkout master
        $ git merge upstream/master
        $ git push origin master
        $ git branch --track [new-branch-name] upstream/[new-branch-name]
        $ git checkout [other-local-branch]
        $ git merge upstream/[other-local-branch]

8. [LOCAL] Merge changes from retrieved upstream master branch into your feature
branch:

        $ git checkout feat-issuename
        $ git merge master

9. [LOCAL - AS NEEDED] If merge notifies of conflicts, determine conflict files.
Edit and correct conflict files.  Flag conflict files as "corrected" by adding
them.  Finish the merge by committing:

        $ git status
        $ [edit and correct conflict files]
        $ git add [conflict files]
        $ git commit

10. [LOCAL] Condense all commits in your feature branch into one single commit:

        $ git rebase -i master

11. [LOCAL] Push your feature branch to your forked repo, and to upstream repo
(if others want to pull it down and test it).  Since you condensed your commits,
you will have to force/overwrite the uncondensed commits that currently exist
on origin and upstream:

        $ git push -f origin feat-issuename
        $ git push -f upstream feat-issuename

12. [BITBUCKET PAGE] Create pull request, specifying additions/changes and issue
number(s):

    * Pull request is FROM your-forked-repo/feat-issuename TO
      upstream-repo/master.

13. [BITBUCKET PAGE] If pull request rejected, begin again from Step #4.

14. [LOCAL] Delete the feature branch locally, from your forked origin repo, and
from upstream repo (if you pushed it to upstream in step 11):

        $ git branch -d feat-issuename
        $ git push origin --delete feat-issuename
        $ git push upstream --delete feat-issuename

### Merging Pull Requests

NOTE: do not merge your own pull requests.

1. [BITBUCKET PAGE] Make sure pull request commentary is properly descriptive.

2. [BITBUCKET PAGE] Review each changed/added line in each source file.

3. [BITBUCKET PAGE] Comment appropriately on specific source code sections.

4. [BITBUCKET PAGE] Merge or reject pull request.

## Testing

### Testing Framework

* PTU uses the C++ [doctest version 1.2.1](https://github.com/onqtam/doctest/tree/1.2.1)
framework for testing functions in source files.  doctest is specified as a git
submodule of the ptu repo.

### Installing Testing Framework

* Install required C++ dependency:

    * For Fedora, install `gcc-c++`.
    * For Ubuntu, install `g++`.
    * For CentOS, first install `epel-release` (installs repository).  Next,
      install `gcc-c++` and `cmake3`.  Lastly, create a link to the newer cmake
      with `ln -s /usr/bin/cmake3 /usr/bin/cmake`.
    * For Antergos, C++ is installed automatically with `gcc`.

* The doctest submodule is automatically downloaded when ptu is cloned, as long
as the `--recursive` argument to the clone command is used.

* If you have cloned ptu __without__ the `--recursive` argument, initialize and
update the doctest submodule from within the provenance-to-use/tests/doctest
directory:

        $ cd provenance-to-use/tests/doctest
        $ git submodule init
        $ git submodule update

### Updating Testing Framework

* To update the doctest submodule to a newer doctest version, show the version
changes, and commit the doctest submodule changes to the main ptu repo:

        $ cd provenance-to-use
        $ git submodule update --remote doctest
        $ git diff
        $ git commit

### Building PTU With Test Harness

* To build the normal `ptu` release version executable with debugging info,
along with the `ptutest` executable that serves as the test harness, run the
build script without any options:

        $ cd provenance-to-use
        $ ./run.sh

### Writing Tests

1. If you created a new module as a new source file, ensure it is placed within
the ptu source tree.

2. Since ptu modules are C modules, but doctest is C++, ensure the header of
module to be tested is wrapped properly.  See an existing tested module header
for an example.

3. Add a new C++ unit test source file (for the module to be tested) in the
`tests` directory.

4. Write tests in the new unit test source file.  See existing unit test source
files for examples and consult the doctest documentation.  Note that tests are
written __only__ for the interface/header of the module to be tested.

### Running Tests

* To run all unit tests:

        $ /path/to/provenance-to-use/ptutest

* To run unit tests for a specific ptu module, or to run certain subtests
within a module, consult the doctest documentation.

* The following example runs all tests for a specific module:

        $ /path/to/provenance-to-use/ptutest --source-file="*mymodule_test*"

* The following example runs all the tests for a specific function within a
module:

        $ /path/to/provenance-to-use/ptutest --test-case="myfunction"

* The following example runs a specific test for a function within a module:

        $ /path/to/provenance-to-use/ptutest --test-case="myfunction" --subcase="mytestname"

## Project Team

TODO

## License

Distributed under the GPLv3 license.

