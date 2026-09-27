#!/bin/sh
echo Exp1
python ./process.py hello.txt out.from.exp1.txt
md5sum out.from.exp1.txt
echo Exp1.done
