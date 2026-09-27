#!/bin/sh
rm -rf cde-package
rm -f provenance.*log
../../ptu $@ ./start.sh 0
../../scripts/db2dot.py -f cde-package/provenance.cde-root.1.log
