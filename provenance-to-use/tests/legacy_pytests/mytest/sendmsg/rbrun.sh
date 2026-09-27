#!/bin/sh
cp ../../ptu cde-package/cde-exec
rm -rf cde-package/provenance.cde-root.1.log_db.*

### rerun
cd cde-package/cde-root/home/quanpt/assi/cde/mytest/sendmsg
echo "server"
../../../../../../../cde-exec $@ -N provenance.cde-root.1.log_db ./recvmsg
#~ ../../../../../../../cde-exec $@ -N provenance.cde-root.1.log_db ./msg s &
#~ sleep 3
#~ killall ld-linux-x86-64.so.2
