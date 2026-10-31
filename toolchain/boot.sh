#!/bin/sh
# Static radio launcher for the Chumby One.
#
# Started in the background at boot, either by the installed hook
# (/psp/rfs1/userhook1 -> /mnt/storage/static/boot.sh) or from a USB stick
# (/mnt/usb/userhook1 -> /mnt/usb/static/boot.sh). The stick's hook runs first,
# so a plugged-in stick wins; the second copy sees the lock and steps aside.
#
# It starts the client right away, stops the stock Flash UI and its services
# as they appear, and keeps the client running. When the client exits it may
# leave a request in /tmp/static-request (from the system menu):
#   stock      open the chumby's own settings until the next restart
#   install    copy this (stick) copy to /mnt/storage/static and add the boot hook
#   update     swap in the staged update from /mnt/storage/static/update/stage
#   uninstall  remove the hook and the program (faves are kept), back to stock
#
# To go back to stock by hand: create /mnt/storage/static/disabled (or delete
# /psp/rfs1/userhook1) and reboot.
P=$(cd "$(dirname "$0")" && pwd)   # program: here (storage or stick)
D=/mnt/storage/static              # data + installed copy
HOOK=/psp/rfs1/userhook1
LOG=/tmp/static-boot.log
REQ=/tmp/static-request
LOCK=/tmp/static-boot.lock
FILES="static-radio ffmpeg cacert.pem boot.sh VERSION README.txt"
CP=/usr/chumby/scripts

[ -x "$P/static-radio" ] || exit 0
[ -e "$D/disabled" ] && exit 0
if [ "$1" != respawn ]; then
  mkdir "$LOCK" 2>/dev/null || exit 0   # another copy (stick or installed) is running
fi
# First boot with no Wi-Fi set up yet: leave the stock setup screens alone.
if [ ! -e /psp/network_config ]; then
  echo "$(date) no Wi-Fi configured yet; leaving the stock UI to set it up" >> $LOG
  rmdir "$LOCK" 2>/dev/null
  exit 0
fi
mkdir -p "$D"
rm -f "$REQ"

stop_stock() {
  [ -e /tmp/static-stock ] && return
  # any Flash player: the boot animation (nice -20) or the control panel
  if pidof chumbyflashplayer.x >/dev/null; then
    $CP/stop_control_panel >/dev/null 2>&1
    killall chumbyflashplayer.x 2>/dev/null
    killall -CONT metadb 2>/dev/null   # the boot animation leaves it SIGSTOPped
  fi
  for svc in chumbradiod chumbhowld; do
    pidof $svc >/dev/null && { $CP/service_control $svc stop >/dev/null 2>&1 || killall $svc 2>/dev/null; }
  done
  killall btplayd acceld 2>/dev/null
}

back_to_stock() {
  touch /tmp/static-stock   # until the next restart
  $CP/start_control_panel >/dev/null 2>&1 &
  exit 0
}

# Let the last client's aplay (and ALSA's dmix server, also an aplay) finish
# closing the sound device first: opening it mid-teardown can wedge this kernel's
# audio driver until a reboot.
wait_audio_idle() {
  i=0
  while pidof aplay >/dev/null && [ $i -lt 10 ]; do sleep 0.5; i=$((i+1)); done
}

# copy a file into place atomically (never overwrite a running script or binary in place)
put() { cp "$1" "$2.new" && chmod "$3" "$2.new" && mv "$2.new" "$2"; }

write_hook() {
  mkdir -p /psp/rfs1
  if [ -e $HOOK ] && ! grep -q "Static radio" $HOOK; then mv $HOOK $HOOK.pre-static; fi
  cat > $HOOK.new <<'EOF'
#!/bin/sh
# Static radio: background launcher (see /mnt/storage/static/boot.sh)
[ -x /psp/rfs1/userhook1.pre-static ] && /psp/rfs1/userhook1.pre-static
/mnt/storage/static/boot.sh >/dev/null 2>&1 &
EOF
  chmod 755 $HOOK.new && mv $HOOK.new $HOOK
}

install_here() {
  echo "$(date) installing $(cat "$P/VERSION") from $P" >> $LOG
  for f in $FILES; do [ -e "$P/$f" ] && put "$P/$f" "$D/$f" 755; done
  rm -rf "$D/LICENSES"; cp -r "$P/LICENSES" "$D/LICENSES" 2>/dev/null
  rm -rf "$D/update"
  write_hook
  sync
  exec "$D/boot.sh" respawn   # carry on from storage, so the stick can come out
}

apply_update() {
  S=$D/update/stage/static
  [ -x "$S/static-radio" ] || { rm -rf "$D/update/stage" "$D/update/ready"; return; }
  echo "$(date) updating $(cat "$D/VERSION" 2>/dev/null) -> $(cat "$S/VERSION")" >> $LOG
  cp "$D/static-radio" "$D/static-radio.prev.new" && mv "$D/static-radio.prev.new" "$D/static-radio.prev"
  cat "$S/VERSION" > "$D/update/applied"
  for f in $FILES; do [ -e "$S/$f" ] && put "$S/$f" "$D/$f" 755; done
  [ -d "$S/LICENSES" ] && { rm -rf "$D/LICENSES"; cp -r "$S/LICENSES" "$D/LICENSES"; }
  rm -rf "$D/update/stage" "$D/update/ready"
  sync
  exec "$D/boot.sh" respawn   # the update may carry a new boot.sh
}

roll_back() {
  echo "$(date) $(cat "$D/update/applied") keeps failing; going back to the previous build" >> $LOG
  cat "$D/update/applied" > "$D/update/skip"   # don't fetch it again
  mv "$D/static-radio.prev" "$D/static-radio"
  rm -f "$D/update/applied"
}

uninstall() {
  echo "$(date) turning Static off" >> $LOG
  rm -f $HOOK
  [ -e $HOOK.pre-static ] && mv $HOOK.pre-static $HOOK
  cd "$D" && rm -rf static-radio static-radio.prev ffmpeg ffprobe cacert.pem VERSION README.txt LICENSES \
    update art moov schedule-cache.json last_station volume boot.sh   # faves.tsv stays
  sync
  back_to_stock
}

if [ "$1" != respawn ]; then
  # The stock UI starts right after the boot hooks. Don't wait for it: start
  # Static now (splash up in seconds) and stop the stock pieces as they appear.
  (
    i=0
    while [ $i -lt 240 ] && [ ! -e "$D/disabled" ] && [ ! -e /tmp/static-stock ]; do   # ~2 minutes of boot
      stop_stock
      sleep 0.5; i=$((i+1))
    done
  ) &
  # an update staged before a restart goes in now
  [ "$P" = "$D" ] && [ -e "$D/update/ready" ] && apply_update
fi
echo "$(date) starting client from $P" >> $LOG

# Keep the client running. If it crashes 3 times in 2 minutes right after an
# update, put the previous build back; 5 times otherwise, hand back to stock.
fails=0; start=$(date +%s)
while [ ! -e "$D/disabled" ]; do
  stop_stock   # something relaunched the stock UI (e.g. an alarm)
  wait_audio_idle
  ran=$(date +%s)
  "$P/static-radio" >> $LOG 2>&1
  now=$(date +%s)
  if [ -e "$REQ" ]; then
    r=$(cat "$REQ"); rm -f "$REQ"
    case "$r" in
      stock) back_to_stock ;;
      install) [ "$P" != "$D" ] && install_here ;;
      update) [ "$P" = "$D" ] && apply_update ;;
      uninstall) uninstall ;;
    esac
    fails=0; start=$now
    continue
  fi
  # ran a while: the update is good
  [ $((now - ran)) -ge 120 ] && rm -f "$D/update/applied"
  if [ $((now - start)) -lt 120 ]; then fails=$((fails+1)); else fails=1; start=$now; fi
  if [ $fails -ge 3 ] && [ "$P" = "$D" ] && [ -e "$D/update/applied" ] && [ -x "$D/static-radio.prev" ]; then
    roll_back; fails=0; start=$now; continue
  fi
  if [ $fails -ge 5 ]; then
    echo "$(date) client keeps failing; restoring stock control panel" >> $LOG
    back_to_stock
  fi
  sleep 2
done
