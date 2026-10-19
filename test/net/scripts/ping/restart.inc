# --- Special > Restart (the disk stays inserted; in-session, so PRAM survives)
mousedown 178 9
wait for 1s
move 190 107
wait for 1s
mouseup
wait text "Welcome to Macintosh" 60s
wait text "Special" 120s
wait for 5s
screenshot "after_restart"
