#!/bin/sh

### prepare clean db and newest binary
rm -rf cde-package/
#cp ../../ptu cde-package/cde-exec

### start server
#../../ptu $@ ./msg s &
../../ptu $@ ./recvmsg &
sleep 0.5

## start client
#~ ../../ptu $@ ./msg c
#~ killall msg
../../ptu $@ ./sendmsg


### post-process db for indirect (spawn) links
../../scripts/db2dot.py -f cde-package/provenance.cde-root.1.log -d gv1 > /dev/null 2>&1
