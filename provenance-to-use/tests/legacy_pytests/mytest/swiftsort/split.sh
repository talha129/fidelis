#!/bin/sh
#./split.sh data.txt 32 data
MYPWD=`pwd`
WD=$3
cd $WD
WCL=`wc -l $1 | sed 's/ [^ ]*$//'`
NUM=`echo $WCL $2 | gawk '{print int(($1-1)/$2)+1 }'`
csplit -s -k -n 10 -f sdata $1 $NUM {*} 2>/dev/null
cd ../
ls $WD/sdata*
