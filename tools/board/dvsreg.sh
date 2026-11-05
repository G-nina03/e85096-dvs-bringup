# during-stream sensor register snapshot: dvsreg.sh OUT
(setsid python3 /usr/local/bin/ov5647-cap.py --raw8 -W 8192 -H 127 -n 3 -B 8 -t 4 -o /tmp/dvs >/tmp/cap.log 2>&1 </dev/null &)
sleep 2
{ for r in 30a0 30a2 30a3 3000 3915 3901 3910; do
    printf '%s: ' $r; i2ctransfer -f -y 7 w2@0x10 0x${r%??} 0x${r#??} r1; done
  echo -n '30a0..: '; i2ctransfer -f -y 7 w2@0x10 0x30 0xa0 r16; } > $1 2>&1
