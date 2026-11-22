# ONEWWW web browser

ONEWWW is a web browser for an Apple II+ or Apple //e with an Appletini ONE.
The card gets each page, lays it out, draws it and sends the picture to its
HDMI output. The Apple program is small: it sends the keys and the mouse to
the card.

## What you need

- Firmware **F1.3.9** or later.
- The disk `ONEWWW.po` from Appletini Software 1.3.9. It boots to the
  browser. `ONEWWW.SYSTEM` is a ProDOS SYS file of 591 bytes, with 6502 code
  only.
- Ethernet with a link and an IP address.
- A display on the HDMI output of the card. The video output of the Apple
  does not show the page.
- A mouse card is optional.

## The screen

The page shows at 640 x 400 in true colour, in place of the Apple picture.
The last rows are a status line: the page title or the address of the link
under the pointer, and the position in the page.

The fonts are DejaVu Sans (regular, bold, italic) and DejaVu Sans Mono, with
smooth edges. Latin, Latin Extended-A, Greek and Cyrillic letters show.

## Keys

| Key | Function |
|---|---|
| G | Go to an address. Type it, then press Return. Escape cancels. |
| Space, P | One page down, one page up |
| Down or J, Up or K | Some rows down or up |
| T, E | Top and end of the page |
| Tab or Right, Left | Select the next or the previous link |
| Return | Open the selected link |
| B | Back to the page before |
| R, H | Load the page again, show the home page |
| Escape | Stop a page that is loading |
| Q | Quit to ProDOS |

With a mouse, point at a link and click.

## What the browser does

- **Pages:** HTTP and HTTPS, gzip, redirects. A page shows first with a
  text in place of each picture; the pictures come after that.
- **Layout:** blocks, lists, tables, floats, flex rows, margins, padding,
  borders, widths, text alignment and backgrounds.
- **Style sheets:** `<style>`, `style` attributes and up to 12 linked
  sheets, with the cascade, selectors, custom properties with `var()`,
  `clamp()`, `min()` and `max()`.
- **Pictures:** PNG, JPEG and GIF, in full colour, at the size that the
  page gives.
- **Forms:** text fields, check boxes, radio buttons and lists, with GET
  and POST. The fields are in the list of links: select one and press
  Return, or click it.
- **Cookies:** the card keeps them while the browser is open, so a login
  stays open.
- **History:** 16 pages.

## Limits

- **JavaScript is not supported.** A page that needs JavaScript shows only a
  part of its content, or no content.
- CSS grid, positioned boxes, animations and web fonts are not supported.
- WebP and SVG pictures do not show.
- The card keeps the cookies in memory. They are gone after Q.
- A POST form sends 2,047 bytes at most.
- The Apple IIgs has no client program yet.
- FTP sharing and the browser use Ethernet one at a time.

## How the Apple program talks to the card

The program uses the SmartPort entry of the card in slot 7, unit 0, with the
private selector `$88`.

| Call | Payload | Function |
|---|---|---|
| CONTROL, operation 1 | version | Open the browser |
| CONTROL, operation 2 | version | Close the browser |
| CONTROL, operation 3 | version, key | Send one key |
| CONTROL, operation 4 | version, x (2 bytes), y (2 bytes), button | Send the mouse position, 0..639 and 0..399 |
| STATUS | | Returns `ATNW`, the version and the state |

The state byte is 0 when the browser is closed. The program reads it after
each key and quits to ProDOS when it is 0.
