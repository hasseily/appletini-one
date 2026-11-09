# CoGS games on Appletini

Open **Ethernet → CoGS games** from the Appletini boot menu or standalone ONE//e menu. Ethernet needs a link and an IP address. Use Up and Down to choose a game, Left and Right to change pages, and Return or A to download. Esc or B closes the browser and cancels an in-progress request.

The browser fetches the 8-bit catalog on first open and keeps its downloadable game disks in memory until the card reboots. Replay collections each have one entry for the full disk. The browser skips individual titles inside collections. Each screen shows up to 20 entries, with the current page and total in the footer. Changing pages or reopening the browser uses the same list. Return or A downloads the selected disk image. [CoGS catalog format](https://a2cogs.com/docs/techref.md)

Appletini creates `_software/CoGS` on the SD card as needed. It checks the byte count before publishing a disk image and gives a new download a separate name when the original name is already in use.
