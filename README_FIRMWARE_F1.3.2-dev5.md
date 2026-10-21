# Firmware F1.3.2-dev5

This development image changes the TransWarp tab of the configuration menu.
It keeps the dev3 FPGA bitstream and XSA and the dev4 soft-switch phase fix.

ONE//e always enables vTW. The TransWarp tab showed "Accelerate the Apple II"
dimmed but not checked, because the row showed the saved setting. While
ONE//e runs, the row now shows "Accelerate the Apple II (ONE//e)", checked and
dimmed. This row comes before the IIgs and boot-mode rows, because a previous
physical host does not apply to ONE//e. Selecting the row shows "ONE//e ALWAYS
ENABLES TRANSWARP" and does not change the saved setting.

`test_vtw.py` checks the row order, the checked state and the toggle lock.
