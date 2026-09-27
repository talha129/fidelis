#!/bin/sh
n=10
rm cde.time.out
for i in $(seq 1 $n)
do
rm provenance.*log swift.log
rm -rf sort-* cde-package/* _concurrent
rm -rf /var/tmp/quanpt/swifttmp/sort-*
cd data
rm *.sorted.*
rm sdata00000000*
rm final*
cd ..
time -a -f "%e %U %S" -o cde.time.out ../../cde/cde swift -recompile -tc.file tc.data sort.swift > stdout.txt 2> stderr.txt
#swift -recompile -tc.file tc.data sort.swift
done
echo n = $n >> cde.time.out
