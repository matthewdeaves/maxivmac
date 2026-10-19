# --- Control Panel (Apple menu, y=123); scroll so MacTCP is at y=100 and Network at y=200
mousedown 22 9
wait for 1s
move 60 123
wait for 1s
mouseup
wait text "Desktop Pattern" 60s
wait for 2s
click 176 200 hold 300ms
wait for 3s
click 176 285 hold 300ms
wait for 3s
click 176 285 hold 300ms
wait for 3s
