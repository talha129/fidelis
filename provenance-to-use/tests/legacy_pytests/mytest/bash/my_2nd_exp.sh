#!/bin/sh
echo Exp2
./s02.rerun_1st_exp.sh
cwd=`pwd`
cp cde-package/cde-root-Y65bDAhpeDT_Z6fa9m4LFtO/$cwd/out.from.exp1.txt ./in.for.exp2.txt
md5sum in.for.exp2.txt
echo Exp2.done
