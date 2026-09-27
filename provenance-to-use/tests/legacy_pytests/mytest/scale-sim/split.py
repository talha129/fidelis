#!/usr/bin/env python2

import os

f = open('input.txt', 'r')
try:
  os.mkdir('input')
except OSError:
  pass # directory exists

for line in f:
  line = line.rstrip()
  fout = open('input/' + line + '.txt', 'w')
  fout.write(line)
  fout.close()
f.close()

try:
  os.mkdir('output')
except OSError:
  pass # directory exists

