#!/bin/sh
~/bin/mpich/bin/mpirun -np 2 -launcher ssh -f host_file ./mpi_hello_world
