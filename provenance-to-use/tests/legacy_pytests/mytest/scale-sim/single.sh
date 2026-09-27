#!/bin/sh

l=$1
scp -q input/$l.txt $l:/var/tmp/
ssh $l ~/assi/cde/mytest/scale-sim/analyze.py /var/tmp/$l.txt /var/tmp/$l.out.txt
scp -q $l:/var/tmp/$l.out.txt output/
echo $l >> barrier.txt
