# --- Network cdev (SE list: after scrolling to the end the last four cdevs are
# Mouse, Network, Sound, Startup Device, so Network is the second icon, y=140).
# MacTCP must NOT be in the System Folder yet (system heap too small otherwise).
click 133 140 hold 300ms
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
click 107 42 hold 300ms
wait for 5s
