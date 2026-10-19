# --- MacTCP cdev: open it, choose "Ethernet Built-In", then More...
click 133 100 hold 300ms
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
