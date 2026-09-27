#!/usr/bin/env python2

import sys, time, random

if len(sys.argv) != 3:
  print sys.argv[0] + ' input output'

inputname = sys.argv[1]
outputname = sys.argv[2]

fout = open(outputname, 'w')

f = open(inputname, 'r')
for line in f:
  fout.write(line[::-1]) # reverse the string
f.close()

random.seed(inputname)
secs = 30 + random.randrange(20)
secs = 30
time.sleep(secs)

fout.close()
