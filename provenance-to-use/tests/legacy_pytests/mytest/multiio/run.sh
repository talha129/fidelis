#!/bin/sh

### prepare clean db and newest binary
rm -rf cde-package

### start
../../ptu $@ ./multiio

### post-process db for indirect (spawn) links
../../scripts/db2dot.py -f cde-package/provenance.cde-root.1.log -d gv1 > /dev/null 2>&1
