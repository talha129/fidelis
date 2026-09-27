#!/bin/sh
cd cde-package
PROV2DOT=../../../scripts/prov2dot.py
#PROV2DOT=../../../scripts/db2dot.py
if [ ! -d gv ]
then
  $PROV2DOT -f provenance.cde-root-Y6*.1.log --withgraph
  cat provenance.cde-root-YP*.1.log  provenance.cde-root-Y6*.2.log > prov.log
  $PROV2DOT -f prov.log -d gv2
  $PROV2DOT -f provenance.cde-root-Y6*.2.log -d gv3 --withgraph
  $PROV2DOT -f provenance.cde-root-YP*.1.log -d gv2 --withgraph
fi
#../../../scripts/graphcmp.py gv/main.graph gv3/main.graph
dot -Tpng gv/main.gv -o exp1.png
dot -Tpng gv2/main.gv -o exp2.png
mv exp*.png ~/html/
