#!/usr/bin/env python2

import sys

if len(sys.argv) != 3:
  print "Usage: python " + sys.argv[0] + " src_file dst_file"
  exit(-1)

try:
  source = open(sys.argv[1], "rb")
except IOError:
  print "Could not open file '" + sys.argv[1] + '"!'
  exit(-1)

try:
  dest = open(sys.argv[2], "wb")
except IOError:
  print "Could not open file '" + sys.argv[2] + '"!'
  exit(-1)

buffer_size=1024*1024
while 1:
  copy_buffer = source.read(buffer_size)
  if copy_buffer:
    dest.write(copy_buffer)
  else:
    break

source.close()
dest.close()
