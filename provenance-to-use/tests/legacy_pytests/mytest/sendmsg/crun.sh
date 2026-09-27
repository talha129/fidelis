#!/bin/sh

### prepare clean db and newest binary
rm -rf cde-package/
#cp ../../ptu cde-package/cde-exec

### start server
../../ptu $@ ./msg a &
sleep 3
killall msg

### post-process db for indirect (spawn) links
../../scripts/db2dot.py -f cde-package/provenance.cde-root.1.log -d gv1 > /dev/null 2>&1
