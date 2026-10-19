# SE variant of launch.inc: at --speed=0 one second is 60 guest ticks (at the
# maximum speed of the Plus run it is a few), so the gap between the two clicks
# of a double-click is 150 ms.
# --- open "Net Tools" (the disk window is open after the restart) and start MacTCP Ping
click 82 105 hold 200ms
wait for 150ms
click 82 105 hold 200ms
wait for 6s
screenshot "n_folder"
click 110 105 hold 200ms
wait for 150ms
click 110 105 hold 200ms
wait text "Start Ping" 60s
wait for 3s
screenshot "n_ping"
