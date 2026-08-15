#!/bin/bash
"$1" "$2" < /dev/null &
PID=$!
MAX=0
while kill -0 $PID 2>/dev/null; do
  RSS=$(grep VmRSS /proc/$PID/status 2>/dev/null | awk '{print $2}')
  [ -n "$RSS" ] && [ "$RSS" -gt "$MAX" ] && MAX=$RSS
  sleep 0.05
done
wait $PID
echo "RSS=$MAX"