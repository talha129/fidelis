#!/bin/sh
cp ../../ptu cde-package/cde-exec
rm -rf cde-package/provenance.cde-root.1.log_db.*

### rerun
#URL="http://128.135.164.139/~quanpt/aaaa.pdf"
URL="http://people.cs.uchicago.edu/~quanpt/aaaa.pdf"
cd cde-package/cde-root/home/quanpt/assi/cde/mytest/wget
rm -f aaaa.pdf
../../../../../../../cde-exec $@ -N provenance.cde-root.1.log_db wget $URL
