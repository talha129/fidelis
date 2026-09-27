#!/bin/sh

### prepare clean db and newest binary
rm -rf cde-package/
cp ../../ptu cde-package/cde-exec

### start server
../../ptu $@ ./s &
#strace ./s &
#valgrind ../../ptu $@ ./s &
#../../ptu ./s &
#./s &
sleep 0.5

### start client
echo "msg1 msg2 hello world msg5 msg6" | ../../ptu ./c > /dev/null 2>&1
#echo "msg1 msg2 hello world msg5 msg6" | ../../ptu ./c > /dev/null
sleep 0.5

### exit server
ps
/bin/echo -ne "_exit" | telnet localhost 8888
sleep 0.5

### post-process db for indirect (spawn) links
../../scripts/db2dot.py -f cde-package/provenance.cde-root.1.log -d gv1 > /dev/null 2>&1
../../scripts/db2dot.py -f cde-package/provenance.cde-root.2.log -d gv2 > /dev/null 2>&1

echo " ==== rerun server ===="
./srerun.sh
echo
echo " ==== rerun client ===="
./crerun.sh
