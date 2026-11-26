#!/bin/sh
# Sole-owner VDEC test: stop the vendor media stack, verify it is gone,
# restart the MPP daemon, run the harness, then bring the vendor stack back.
SV=/usr/usrdata/bin/runit/sv

echo "== clear leftover probes"
killall -9 pd2 poc_dec 2>/dev/null
sleep 1

 echo "== stop vendor stack"
$SV stop /etc/services/media_service
$SV stop /etc/services/pilot
sleep 2
for p in $(ps w | awk '/[m]edia_service/ {print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ps w | awk '/[p]ilot$/ {print $1}'); do kill -9 $p 2>/dev/null; done
sleep 1
echo "-- still alive:"
ps w | awk '/[m]edia_service|[p]ilot$|[m]pp_service.app/ {print $1, $3}' | head -5

echo "== fresh MPP daemon"
for p in $(ps w | awk '/[m]pp_service.app/ {print $1}'); do kill -9 $p 2>/dev/null; done
sleep 1
$SV up /etc/services/mpp_service
sleep 3
ps w | awk '/[m]pp_service.app/ {print "daemon:", $1}' | head -2

echo "== decode (headless)"
cd /tmp || exit 1
LD_LIBRARY_PATH=/usr/lib:/lib ./pd2 /tmp/es.h264 120 > /tmp/dec.out 2>&1 &
P=$!
sleep 15
kill -9 $P 2>/dev/null
sleep 1
cat /tmp/dec.out

echo "== restore vendor stack"
$SV start /etc/services/media_service
$SV start /etc/services/pilot
sleep 4
ps w | awk '/[m]edia_service|[p]ilot$/ {print $1, $3}' | head -4
echo "== done"
