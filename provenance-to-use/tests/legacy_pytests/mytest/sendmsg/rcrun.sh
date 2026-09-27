#!/bin/bash
AWD=cde-package/cde-root/home/quanpt/assi/cde/mytest/sendmsg

cp ../../ptu cde-package/cde-exec
rm -rf cde-package/provenance.cde-root.1.log_db.*
cp ./msg $AWD/

### rerun
cd $AWD
../../../../../../../cde-exec $@ -N provenance.cde-root.1.log_db ./msg a &
cdepid=$!
sleep 4

kill -9 $cdepid >/dev/null 2>&1
ps ef | grep "cde-package/cde-root/lib64/ld-linux" | awk '{print $1}' | while read l; do kill -9 $l > /dev/null 2>&1; done
ps ef | grep "<defunct>" | awk '{print $1}' | while read l; do kill -9 $l > /dev/null 2>&1; done
