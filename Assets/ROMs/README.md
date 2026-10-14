# Video Character ROM Override

Appletini renders Apple //e text with the built-in Enhanced US character ROM
(342-0265-A with MouseText). A different //e video character ROM can be loaded
from the SD card through the Video tab.

Accepted files must meet both requirements:

1. The size is 4096 bytes, or 8192 bytes for a dual-charset part. For an
   8192-byte file, Appletini uses the primary 4 KB bank.
2. The data is not entirely `00` or entirely `FF`.

Missing or invalid files select the built-in Enhanced US ROM.

Apple II programs can also upload a complete 4 KB ROM through the
[SmartPort glyph API](../../README_GLYPH_API.md), selector `$81` on controller
unit 0. Uploads change Appletini's rendered text. They do not change the
motherboard's character ROM or the saved `video.rom` setting. RESTORE returns
to this menu selection, and selecting a ROM in the menu cancels an upload.
