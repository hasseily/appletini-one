# Video Character ROM Override

Appletini renders Apple //e text with the built-in Enhanced US character ROM
(342-0265-A with MouseText). A different //e video character ROM can be loaded
from the SD card through the Video tab.

Accepted files must meet both requirements:

1. The size is 4096 bytes, or 8192 bytes for a dual-charset part. For an
   8192-byte file, Appletini uses the primary 4 KB bank.
2. The data is not entirely `00` or entirely `FF`.

Missing or invalid files select the built-in Enhanced US ROM.
