# --- Drag MacTCP from the volume root into the System Folder (icons in the
# "Macintosh" window opened from its disk icon)
click 473 45 hold 200ms
wait for 1s
click 473 45 hold 200ms
wait for 8s
drag 210 105 146 105 hold 1s
wait for 6s
screenshot "moved_root"
