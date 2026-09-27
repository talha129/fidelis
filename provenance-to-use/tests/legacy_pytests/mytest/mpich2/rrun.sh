#!/bin/sh
cp ../../ptu cde-package/cde-exec
rm -rf cde-package/provenance.cde-root.1.log_db.*

### rerun
cd cde-package/cde-root/home/quanpt/assi/cde/mytest/mpich2
../../../../../../../cde-exec $@ -N provenance.cde-root.1.log_db ./exp.sh
#../../../../../../../cde-exec $@ ./exp.sh
