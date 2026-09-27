#!/usr/bin/env python2

import os

try:
  os.mkdir('/home/pgbovine/testdir')
except OSError:
  pass

