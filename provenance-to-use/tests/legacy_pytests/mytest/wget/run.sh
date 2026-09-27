#!/bin/sh

### prepare clean db and newest binary
rm -rf cde-package
rm spim.pdf*
#cp ../../ptu cde-package/cde-exec

### start
#URL="http://128.135.164.139/~quanpt/spim.pdf"
URL="http://people.cs.uchicago.edu/~quanpt/spim.pdf"
../../ptu $@ wget $URL

### post-process db for indirect (spawn) links
../../scripts/db2dot.py -f cde-package/provenance.cde-root.1.log -d gv1 > /dev/null 2>&1

echo " ==== rerun wget ===="
./rrun.sh
