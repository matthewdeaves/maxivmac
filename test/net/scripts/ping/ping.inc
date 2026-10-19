# --- ping the slirp gateway: type the address, press Ping, wait for the run
click 310 70 hold 300ms
wait for 2s
type "10.0.2.2"
wait for 3s
screenshot "z1_typed"
click 345 306 hold 300ms
# the window ends the run with the tally "Packets out/in/bad/%loss = 5/5/0/0";
# with lost replies this expectation fails: the message names this line and
# lists the text the Ping window did draw
expect text "5/5/0/0" 300s
wait for 3s
screenshot "z1_gw_done"
