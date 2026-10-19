# --- MacTCP "More..." dialog and main panel: manual address 10.0.2.15/24, gateway 10.0.2.2,
# DNS 10.0.2.3 (domain example.net: MacTCP refuses a DNS entry with a blank domain).
# The subnet slider needs stepwise moves (the cursor lags a single drag).
mousedown 276 108
wait for 1s
move 380 108
wait for 1s
move 381 108
wait for 700ms
move 382 108
wait for 700ms
move 383 108
wait for 700ms
move 384 108
wait for 700ms
move 385 108
wait for 700ms
mouseup
wait for 1s
screenshot "m3_slider"
# IP address fields: 10 . 0 . 2 . 15
click 320 172 hold 300ms
wait for 1s
key delete
key delete
key delete
key delete
key delete
key delete
type "10"
key tab
wait for 1s
key delete
key delete
key delete
key delete
key delete
key delete
type "2"
key tab
wait for 1s
key delete
key delete
key delete
key delete
key delete
key delete
type "15"
wait for 2s
# Gateway
click 115 243 hold 300ms
wait for 1s
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
type "10.0.2.2"
wait for 1s
# DNS domain and server
click 255 271 hold 300ms
wait for 1s
type "example.net"
wait for 1s
click 360 271 hold 300ms
wait for 1s
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
key delete
type "10.0.2.3"
wait for 1s
click 437 271 hold 300ms
wait for 2s
screenshot "m3_fields"
# OK on More..., then the panel's close box (writes "MacTCP Prep")
click 75 311 hold 300ms
wait for 6s
screenshot "m4_ok"
click 70 47 hold 300ms
wait for 6s
screenshot "m4_closed"
