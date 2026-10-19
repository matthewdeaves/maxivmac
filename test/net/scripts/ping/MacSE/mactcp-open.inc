# --- MacTCP cdev: the SE list has one more entry (Startup Device) than the Plus,
# so MacTCP is not visible at the end of the list; one step up (the arrow moves
# two entries) shows Keyboard, MacTCP, Mouse, Network: MacTCP at y=140.
click 176 62 hold 300ms
wait for 3s
click 133 140 hold 300ms
wait text "Ethernet Built-In" 60s
wait for 3s
screenshot "m1_open"
click 340 70 hold 300ms
wait for 4s
screenshot "m2_sel"
click 297 285 hold 300ms
wait for 6s
click 66 57 hold 300ms
wait for 4s
screenshot "m2_more"
