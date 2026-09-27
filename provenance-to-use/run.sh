#!/bin/sh

################################################################################
# script:   run.sh
# author:   digimokan
# date:     10 AUG 2017 (created)
# purpose:  main entry point for building ptu, optionally build test harness
# usage:    see below usage() function
################################################################################

# build output dir, output executable name(s), build types
builddir='build'
exec_name='ptu'
exec_testing_name='ptutest'
execoutputs="${exec_name} ${exec_testing_name}"
build_release='-DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release'
build_with_tests='-DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo'
buildtype="${build_with_tests}"

# print cmd usage and options to stdout
usage() {
  echo 'Usage: run.sh [-h] [-r]'
  echo '  -h      print this help message'
  echo '  -r      do optimized release build, do not build tests'
}

# get cli args and check their validity
args=`getopt hr $*`
if [ $? -ne 0 ]; then
  usage
  exit 2
fi

# load cli args into array
set -- ${args}

# parse cli args
while [ $# -ne 0 ]; do
  case "$1" in
    -h)   usage;
          exit 0;;
    -r)   buildtype="${build_release}";
          execoutputs="${exec_name}";
          shift;;
    --)   shift;
          break;;
  esac
done

# make and change to "build" dir
mkdir -p ${builddir}
cd ${builddir}

# run cmake in source root dir, and output Makefile/etc to build dir
cmake ${buildtype} ..

# compile sources, and output to build dir
make

# move compiled executable(s) to source root dir
mv ${execoutputs} ..
cd ..

