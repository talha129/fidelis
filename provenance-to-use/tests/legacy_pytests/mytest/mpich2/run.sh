#!/bin/sh
rm -rf cde-package

#mpirun -n 2  ./mpi_hello_world
../../ptu $@ ./exp.sh

### post-process db for indirect (spawn) links
../../scripts/db2dot.py -f cde-package/provenance.cde-root.1.log -d gv1 > /dev/null 2>&1
