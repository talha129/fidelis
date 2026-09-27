#!/bin/sh
cp ../../ptu cde-package/cde-exec
rm -rf cde-package/provenance.cde-root.1.log_db.*

### rerun
cd cde-package/cde-root/home/quanpt/assi/cde/mytest/sendmsg
echo "server"
../../../../../../../cde-exec $@ -N provenance.cde-root.1.log_db ./msg s

echo "client"
../../../../../../../cde-exec $@ -N provenance.cde-root.2.log_db ./msg c
