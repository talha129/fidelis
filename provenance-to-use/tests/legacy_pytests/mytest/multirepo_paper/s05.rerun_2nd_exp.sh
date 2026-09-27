#!/bin/sh
cd cde-package
if [ ! -f 2nd_exp.sh ]
then
  mv cde.log 2nd_exp.sh
fi
sh 2nd_exp.sh
