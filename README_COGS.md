# CoGS games on Appletini

Open **Ethernet → CoGS games** from the Appletini boot menu or standalone ONE//e menu. Ethernet needs a link and an IP address. Use Up and Down to choose a game, Left and Right to change pages, and Return or A to download. Esc or B closes the browser and cancels an in-progress request.

The browser requests the 8-bit games category and checks the platform and category in every result before showing it. Some games are listed individually inside collections such as Total Replay. Appletini saves the collection disk for those entries. Open the disk with Appletini’s file picker and choose the game from the collection’s own menu. [CoGS catalog format](https://a2cogs.com/docs/techref?v=33)

Appletini creates `_software/CoGS` on the SD card as needed. It checks the byte count before publishing a disk image and gives a new download a separate name when the original name is already in use.
