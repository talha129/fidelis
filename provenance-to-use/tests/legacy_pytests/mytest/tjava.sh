#!/bin/sh
cd javart
rm -rf cde-package
../../ptu /usr/bin/java TestExec
../../scripts/db2dot.py -f cde-package/provenance.cde-root.1.log
../../scripts/prov2dot.py -f cde-package/provenance.cde-root.1.log -d gv2
#./printdb.py
