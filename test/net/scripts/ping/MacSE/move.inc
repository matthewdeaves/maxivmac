# SE variant of move.inc: at --speed=0 one second is 60 guest ticks (at the
# maximum speed of the Plus run it is a few), so the gap between the two clicks
# of a double-click is 150 ms.
# --- Drag MacTCP from the volume root into the System Folder (icons in the
# "Macintosh" window opened from its disk icon)
click 473 45 hold 200ms
wait for 150ms
click 473 45 hold 200ms
wait for 8s
drag 210 105 146 105 hold 1s
wait for 6s
screenshot "moved_root"
