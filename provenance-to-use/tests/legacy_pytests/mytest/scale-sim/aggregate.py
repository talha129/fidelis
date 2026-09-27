#!/usr/bin/env python2

from os import listdir
from os.path import isfile, join

fout = open('output.txt', 'w')

mypath = './output'
for f in listdir(mypath):
  if isfile(join(mypath,f)):
    fin = open(join(mypath, f), 'r')
    fout.write(fin.read()+'\n')
    fin.close()

fout.close()
