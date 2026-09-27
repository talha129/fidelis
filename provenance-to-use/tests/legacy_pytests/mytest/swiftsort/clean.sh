#!/bin/sh
rm provenance.*log swift.log
rm -rf sort-* cde-package/* _concurrent
rm -rf /var/tmp/quanpt/swifttmp/sort-*
cd data
rm *.sorted.*
rm sdata00000000*
rm final*
cd ..
#time swift -recompile -tc.file tc.data sort.swift > stdout.txt 2> stderr.txt
#swift -recompile -tc.file tc.data sort.swift
