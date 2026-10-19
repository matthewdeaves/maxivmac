# --- Chooser: switch AppleTalk to Active (Apple menu > Chooser, y=107)
mousedown 22 9
wait for 1s
move 60 107
wait for 1s
mouseup
wait text "AppleTalk" 60s
wait for 3s
click 331 259 hold 300ms
wait text "Continue" 20s
wait for 1s
click 230 207 hold 300ms
wait for 6s
screenshot "chooser_active"
click 70 47 hold 300ms
wait for 3s
