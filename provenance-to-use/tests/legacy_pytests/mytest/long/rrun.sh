#!/bin/sh
cp ../../ptu cde-package/cde-exec
rm -rf cde-package/provenance.cde-root.1.log_db.*

### rerun
cd cde-package/cde-root/home/quanpt/assi/cde/mytest/long
../../../../../../../cde-exec $@ -N provenance.cde-root.1.log_db ./start.sh 0
