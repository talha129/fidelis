#!/bin/sh
cd bash
rm provenance.*log
../cde ./run.sh 
../prov2dot.py
rm -rf ~/html/cdenet/bashrpl
mv gv ~/html/cdenet/bashrpl
