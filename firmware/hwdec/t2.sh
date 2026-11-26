#!/bin/sh
# Scan VB_UID space with the vendor stack stopped.
SV=/usr/usrdata/bin/runit/sv

echo "== stop vendor stack"
$SV stop /etc/services/media_service
$SV stop /etc/services/pilot
sleep 2
for p in $(ps w | awk '/[m]edia_service/ {print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ps w | awk '/[p]ilot$/ {print $1}'); do kill -9 $p 2>/dev/null; done
sleep 1
echo "-- alive:"; ps w | awk '/[m]edia_service|[p]ilot$/ {print $1, $3}' | head -3

echo "== fresh daemon"
for p in $(ps w | awk '/[m]pp_service.app/ {print $1}'); do kill -9 $p 2>/dev/null; done
sleep 1
$SV up /etc/services/mpp_service
sleep 3

echo "== scan"
cd /tmp || exit 1
LD_LIBRARY_PATH=/usr/lib:/lib ./pd3 scan > /tmp/scan.out 2>&1 &
P=$!
sleep 20
kill -9 $P 2>/dev/null
sleep 1
cat /tmp/scan.out

echo "== restore"
$SV start /etc/services/media_service
$SV start /etc/services/pilot
sleep 3
echo "== done"
