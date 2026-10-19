# --- Network cdev: select EtherTalk, confirm, wait for the switch.
# MacTCP must NOT be in the System Folder yet (system heap too small otherwise).
click 133 200 hold 300ms
wait for 10s
screenshot "ether_net"
click 292 105 hold 300ms
wait for 8s
click 362 181 hold 300ms
wait text "Switching your AppleTalk" 60s
# the panel redraws when the switch is done (a failure shows an alert instead,
# and the rest of the script then fails on its own)
wait text "AppleTalk Connection:" 300s
wait for 3s
screenshot "ether_done"
click 70 47 hold 300ms
wait for 5s
