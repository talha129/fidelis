#!/bin/sh

for n in 01 02 05 10 20 40
do
  head -n $n cslist.txt > cslist.ln.txt
  for i in `seq 5`
  do
    ./run.sh >> log.ptu.$n.txt 2>&1
  done
  cat log.ptu.$n.txt | grep real > time.ptu.$n.txt
  awk "{ total += $$2; count++ } END { print total/count }" time.ptu.$n.txt >> time.ptu.$n.txt
done

for n in 01 02 05 10 20 40; do echo -ne $n "\t" >> table.txt; awk '{ total += $$2; count++ } END { print total/count }' time.ptu.$n.txt >> table.txt; done

