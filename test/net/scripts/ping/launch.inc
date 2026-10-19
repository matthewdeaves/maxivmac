# --- open "Net Tools" (the disk window is open after the restart) and start MacTCP Ping
click 82 105 hold 200ms
wait for 1s
click 82 105 hold 200ms
wait for 6s
screenshot "n_folder"
click 110 105 hold 200ms
wait for 1s
click 110 105 hold 200ms
wait text "Start Ping" 60s
wait for 3s
screenshot "n_ping"
