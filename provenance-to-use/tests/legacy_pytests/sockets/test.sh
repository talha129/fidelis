#!/bin/bash
PORT=$((RANDOM%1000+10000))
SERVER=gabri.cs.uchicago.edu

echo -e "\n\n=== cdenet ==="
../../cde ./server $PORT &
echo Hello there! | ../../cde ./client $SERVER $PORT
exit

echo -e "\n\n=== strace ==="
PORT=$((RANDOM%1000+10000))
strace ./server $PORT &
echo Hello there! | strace ./client $SERVER $PORT
