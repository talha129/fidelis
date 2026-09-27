#!/bin/sh
cp ../../ptu cde-package/cde-exec
rm -rf cde-package/provenance.cde-root.2.log_db.*

### start server
#./s > /dev/null 2>&1 &
#sleep 0.5

### start client
cd cde-package/cde-root/home/quanpt/assi/cde/mytest/net
echo "msg1 msg2 hello world msg5 msg6" | ../../../../../../../cde-exec -N provenance.cde-root.2.log_db './c' "$@"

### kill remains
#killall s
