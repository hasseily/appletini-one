/******************************************************************************
 * config_menu_help.c -- ALL config-menu help text, in one place.
 *
 * =====================  HOW TO EDIT THIS FILE  =============================
 *
 * The Help panel shows a block of lines that depends on which TAB you are on
 * and, optionally, which ITEM within that tab is highlighted.
 *
 *   - Every tab has a DEFAULT block (shown when no item override applies).
 *   - Any tab may add PER-ITEM overrides: when that item is highlighted, its
 *     block replaces the tab default.
 *
 * To change wording:  edit the strings in the relevant HELP(...) block below.
 * To add a line:      add another "..." string to the block.
 * To give one item its own help:
 *     1. Add a HELP(tabname_itemdesc, "line", "line", ...) block.
 *     2. Add an OVERRIDE(item_index, tabname_itemdesc) line to that tab's
 *        override list (create the list + the ", <list>" argument on the
 *        TAB row if it doesn't have one yet -- see USB for an example).
 *
 * Each display line is one entry. Lines are shown as written (no automatic
 * wrapping), so keep them roughly <= 100 characters to fit the panel. Use an
 * empty string "" for a blank spacer line.
 *
 * The Video and Clock tabs append live, runtime-computed
 * status lines after the text below; that happens in config_menu.c and is
 * intentionally NOT here, because those lines depend on current state.
 * =========================================================================
 ******************************************************************************/

#include "config_menu_help.h"
#include "config_menu_internal.h"   /* for config_tab_t / CONFIG_TAB_* */

/* --- authoring helpers: you should not need to change these --------------- */

#define HELP_COUNT(arr) ((uint32_t)(sizeof(arr) / sizeof((arr)[0])))

/* Declare a named block of help lines. */
#define HELP(name, ...) \
    static const char * const help_##name[] = { __VA_ARGS__ }

/* One per-item override row (inside a tab's override list). */
#define OVERRIDE(item_index, name) \
    { (uint32_t)(item_index), help_##name, HELP_COUNT(help_##name) }

/* A tab row with no per-item overrides. */
#define TAB(tab_id, name) \
    { (tab_id), help_##name, HELP_COUNT(help_##name), NULL, 0U }

/* A tab row that has a per-item override list. */
#define TAB_WITH_OVERRIDES(tab_id, name, ovr_list) \
    { (tab_id), help_##name, HELP_COUNT(help_##name), \
      (ovr_list), HELP_COUNT(ovr_list) }

typedef struct {
    uint32_t             item;
    const char * const  *lines;
    uint32_t             count;
} help_override_t;

typedef struct {
    uint32_t                tab;
    const char * const     *default_lines;
    uint32_t                default_count;
    const help_override_t  *overrides;
    uint32_t                override_count;
} help_tab_t;

/* ======================================================================== */
/*  PROFILES                                                                */
/* ======================================================================== */
HELP(profiles,
    "Profiles store complete menu configurations under 0:/PROFILES, including media paths and video choices.",
    "Choose a profile to load it, save the current working setup, rename it, or attach a PNG thumbnail.",
    "Take screenshots of the Apple display using the USB-owned menu; screenshots are saved to 0:/SCREENSHOTS");

/* ======================================================================== */
/*  BOOT SETTINGS                                                           */
/* ======================================================================== */
HELP(boot_settings,
    "Boot settings control how long the menu prompt appears, which device boots first, and USB menu bindings.",
    "There are two menu modes: Pressing 'A' on the keyboard while booting activates the BOOT mode.",
    "During boot mode, you can use the Apple keyboard to fully navigate the menu and change settings.",
    "In normal operation, pressing the USB device's 'MENU' binding activates the USB mode.",
    "During USB mode, the menu is controlled by the USB bindings. Do not use the Apple keyboard, as it",
    "interacts with the running Apple software. You cannot rebind the USB bindings in USB mode.");

/* ======================================================================== */
/*  VIDEO   (config_menu.c appends a live "PAL Accurate" note)              */
/* ======================================================================== */
HELP(video,
    "Highlight a Video control for details. Changes apply immediately and persist in the active config.",
    "Legacy output, effects, borders, character ROM, bezel, and diagnostics are configured independently.",
    "Performance varies. At best, the Appletini ONE can run at about 125 FPS in full 1080p resolution.",
    "It dips below 60 FPS only with debug, a 1080p bezel, borders, and ghosting all enabled.",
    "Ghosting, a phosphor persistence effect across frames, is by far the most expensive feature.");

HELP(video_output,
    "Color renders legacy Apple video with the decoder selected on the next row.",
    "Monochrome removes artifact color and uses the selected White, Green, or Amber tint.",
    "SHR ignores this switch and follows its own $C029 black-and-white control.");

HELP(video_variant,
    "With Monochrome, this row selects the display tint. Add ghosting for a phosphor persistence effect.",
    "With Color output, choose a color model. Idealized and RGB are crisp, but miss color blending.",
    "Composite Monitor and Color TV model analog color artifacts and blending.",
    "PAL Accurate appears only on PAL machines. It models individual signal components.",
    "We're happy to implement an accurate NTSC model if someone can provide the necessary data.");

HELP(video_video7,
    "Video-7 mono watches the $C05E/$C05F soft-switch sequence used by compatible Video-7 software.",
    "When enabled, a certain switch combination will force monochrome output.",
    "Disable it if you see software in monochrome when it should be in color.");

HELP(video_scanlines,
    "Scanlines blank replicated output rows after scaling. It is a naive but effective effect.",
    "There is no performance impact, and it is purely a matter of preference.");

HELP(video_ghosting,
    "Phosphor ghosting retains bright pixels from earlier displayed frames and decays them over time.",
    "Light, Medium, and Strong increase persistence, so motion and flashes leave longer trails.",
    "Ghosting is expensive, so avoid mixing it with borders, full screen bezels and debug.");

HELP(video_border,
    "Enables the IIgs-style border. This is cycle accurate and has moderate performance impact.",
    "From BASIC, POKE 49204,N selects one of 16 colors; only the low nibble is used.");

HELP(video_border_color,
    "This is the power-on and Apple-reset value of the $C034 border latch.",
    "Changing it applies immediately. Software can override it via the $C034 soft switch.",
    "The override lasts until another menu color change or Apple reset restores this value.");

HELP(video_border_outside,
    "Bezel keeps the selected background outside the cycle-accurate border.",
    "Flood paints that area with the border's frame-end color and follows the Scanlines setting.",
    "Flood forces Show bezel and Show debugging Off and disables both controls in Flood mode.");

HELP(video_rom,
    "Select a 4096-byte or 8192-byte Apple //e character ROM from the SD card.",
    "An 8192-byte dual-charset dump uses its first 4096-byte bank; invalid files are rejected.",
    "Built-in uses the enhanced US //e ROM. This changes glyphs, not the Apple system ROM.");

HELP(video_show_bezel,
    "Show bezel paints the selected background around the Apple image; Off leaves a black surround.",
    "Visibility changes immediately and does not alter the Apple video signal or its memory.",
    "Border Flood forces this Off and disables it until Bezel mode is selected again.");

HELP(video_bezel,
    "Choose a PNG exactly 1920 pixels wide and up to 1080 pixels high; it starts at the top left.",
    "The larger the image, the more the performance impact, but it remains moderate.",
    "Auto tries 0:/bezel.png, then 0:/bezels/bezel.png, then the embedded default.",
    "Show bezel controls visibility. Border Flood disables this file selector.");

HELP(video_debug,
    "Show debugging overlays live firmware, video, compositor, USB, and storage diagnostics.",
    "Use it to inspect frame rate, timing, and service state while diagnosing firmware or hardware.",
    "It adds overlay drawing. Border Flood forces it Off and disables this control.",
    "Showing debugging has a mild performance impact.",
    "Let us know if you want additional debug information.");

static const help_override_t video_overrides[] = {
    OVERRIDE(CONFIG_VIDEO_ITEM_OUTPUT, video_output),
    OVERRIDE(CONFIG_VIDEO_ITEM_VARIANT, video_variant),
    OVERRIDE(CONFIG_VIDEO_ITEM_VIDEO7, video_video7),
    OVERRIDE(CONFIG_VIDEO_ITEM_SCANLINES, video_scanlines),
    OVERRIDE(CONFIG_VIDEO_ITEM_GHOSTING, video_ghosting),
    OVERRIDE(CONFIG_VIDEO_ITEM_BORDER, video_border),
    OVERRIDE(CONFIG_VIDEO_ITEM_BORDER_COLOR, video_border_color),
    OVERRIDE(CONFIG_VIDEO_ITEM_BORDER_FLOOD, video_border_outside),
    OVERRIDE(CONFIG_VIDEO_ITEM_ROM, video_rom),
    OVERRIDE(CONFIG_VIDEO_ITEM_SHOW_BEZEL, video_show_bezel),
    OVERRIDE(CONFIG_VIDEO_ITEM_BEZEL, video_bezel),
    OVERRIDE(CONFIG_VIDEO_ITEM_DEBUG, video_debug),
};

/* ======================================================================== */
/*  SMARTPORT                                                               */
/* ======================================================================== */
HELP(smartport,
    "SmartPort presents up to eight block devices as ProDOS/SOS-style mass-storage units.",
    "Each SP row selects, replaces, or clears an image on the SD card; duplicate images are blocked.",
    "Supported: HDV, 2MG, 2IMG, 140K PO, and 800K PO.",
    "SuperSprite shares slot 7; when it is enabled, the rest of this page is disabled.");

HELP(smartport_supersprite,
    "SuperSprite: a TMS9918 VDP plus AY-3-8910 (3-voice PSG) sound card. It renders sprites and tile",
    "graphics as an overlay on the Apple video and adds PSG sound.",
    "It lives in SLOT 7, which is the SmartPort slot -- enabling it disables the SmartPort disk rows",
    "and RAM32 until you turn SuperSprite off again.",
    "To use it: boot first (Disk II or SmartPort), then enable SuperSprite to run the VDP Software.",
    "Turn it back off to restore SmartPort. Changes apply immediately.");

HELP(smartport_ram32,
    "RAM32 is a volatile 32MB SmartPort block device backed by Appletini RAM.",
    "It appears as an additional SmartPort disk and is useful as a fast scratch disk.",
    "Its contents are not saved to SD and are lost when power is removed or the feature is disabled.",
    "It is independent of RamWorks-style Apple II memory in the RAM tab.");

static const help_override_t smartport_overrides[] = {
    OVERRIDE(SMARTPORT_DEVICE_COUNT + 1U, smartport_ram32),
    OVERRIDE(SMARTPORT_DEVICE_COUNT + 2U, smartport_supersprite),
};

/* ======================================================================== */
/*  DISK II                                                                 */
/* ======================================================================== */
HELP(disk2,
    "Disk II emulates slot 6 drives with activity overlay and optional drive-door and activity audio.",
    "Select Disk 1 or Disk 2 to attach or clear an image; write-protected files show a lock.",
    "Supported: WOZ, NIB, DSK, DO, PO.");

/* ======================================================================== */
/*  MOUSE                                                                   */
/* ======================================================================== */
HELP(mouse,
    "Mouse emulates an Apple Mouse Card in slot 2 using USB HID input from USB1.",
    "Sensitivity scales movement before it reaches Apple software; changes apply immediately.",
    "The original //e mouse is extremely slow. Set sensitivity to 12% for a similar feel, but this also",
    "depends heavily on the mouse's DPI. Experiment to find the best sensitivity for your mouse.");

/* ======================================================================== */
/*  MOCKINGBOARD / PHASOR                                                   */
/* ======================================================================== */
HELP(phasor,
    "Phasor sound card: four YM2149 chips, 12 channels. 2x SSI-263/SC-01 speech chips.",
    "The Phasor is essentially 2 Mockingboard cards in one slot. It should run all Mockingboard software.",
    "Pan sliders position the 12 channels; audio sliders tune bass, mid, treble, and volume.",
    "The Appletini's audio is very clean, so you can push the volume up high.");

HELP(phasor_mockingboard_only,
    "Locks the card in plain Mockingboard mode and ignores Phasor $C0nX mode-switch writes.",
    "Only AY0 and AY1 remain active, providing the six channels of a standard Mockingboard.",
    "Both speech chips remain active (when used by software).",
    "Use this when software misdetects a Phasor or behaves incorrectly in native or Echo+ mode.");

HELP(phasor_volume_envelope,
    "Chooses the PSG output-level curve used for fixed-volume and hardware-envelope playback.",
    "Original AY-3-8913 uses the classic 16-level response; YM-2149 has a finer 32-level curve.",
    "This changes loudness steps and timbre, not the envelope shape, period, or timing.");

HELP(phasor_bass,
    "Adjusts low-frequency content in the complete Phasor mix, including PSG and speech audio.",
    "Range is -8 to +8; 0 adds no bass adjustment. Negative values cut and positive values boost.",
    "Strong boosts can clip loud passages, so balance Bass against the Volume control.",
    "TIP: Increase bass substantially, as 8-bit software tends to lack low-end presence."
);

HELP(phasor_mid,
    "Adjusts middle-frequency content in the complete Phasor mix, including PSG and speech audio.",
    "Range is -8 to +8; 0 adds no mid adjustment. Negative values cut and positive values boost.",
    "Cut for a softer sound or boost for more presence; strong boosts can clip loud passages.");

HELP(phasor_treble,
    "Adjusts high-frequency content in the complete Phasor mix, including PSG and speech audio.",
    "Range is -8 to +8; 0 adds no treble adjustment. Negative values cut and positive values boost.",
    "Cut to soften hiss and sharp edges, or boost for clarity; strong boosts can clip.");

HELP(phasor_volume,
    "Adjusts overall gain after PSG panning, speech mixing, and the Bass, Mid, and Treble controls.",
    "Range is -8 to +8; 0 adds no gain. Negative values reduce level and positive values raise it.",
    "Positive gain can saturate the 16-bit output; reduce it if loud passages sound distorted.");

static const help_override_t phasor_overrides[] = {
    OVERRIDE(PHASOR_MOCKINGBOARD_ONLY_FOCUS, phasor_mockingboard_only),
    OVERRIDE(PHASOR_AUDIO_FOCUS_BASE + PHASOR_AUDIO_CONTROL_BASS, phasor_bass),
    OVERRIDE(PHASOR_AUDIO_FOCUS_BASE + PHASOR_AUDIO_CONTROL_MID, phasor_mid),
    OVERRIDE(PHASOR_AUDIO_FOCUS_BASE + PHASOR_AUDIO_CONTROL_TREBLE, phasor_treble),
    OVERRIDE(PHASOR_AUDIO_FOCUS_BASE + PHASOR_AUDIO_CONTROL_VOLUME, phasor_volume),
    OVERRIDE(PHASOR_PSG_MODE_FOCUS, phasor_volume_envelope),
};

/* ======================================================================== */
/*  ETHERNET                                                                */
/* ======================================================================== */
HELP(ethernet,
    "Ethernet enables the slot 1 Uthernet II-compatible W5100S interface.",
    "The Apple sees the C0N0-C0N3 register window while firmware can read/write W5100S network registers.",
    "Use static fields directly, or select DHCP to request a fresh address at each boot.");

HELP(ethernet_config,
    "Configure network at boot writes static fields or runs DHCP, according to Address Mode.",
    "Leave it off if Apple software should own the card registers completely after reset.",
    "A saved DHCP mode always negotiates a new lease instead of replaying the previous address.");

HELP(ethernet_fields,
    "Enter moves to the next byte or octet; Left/Right decrements or increments the selected value.",
    "MAC is saved as six hexadecimal bytes; IP, subnet, and gateway are saved as dotted decimal.");

HELP(ethernet_dhcp,
    "DHCP uses the configured MAC address to request IP, subnet, and gateway from the local network.",
    "A successful lease enables boot configuration; later boots request a fresh lease.");

HELP(ethernet_test,
    "Test link reads the W5100S identity/version, PHY link status, and current IP from the card.");

static const help_override_t ethernet_overrides[] = {
    OVERRIDE(CONFIG_ETHERNET_ITEM_CONFIG_ENABLED, ethernet_config),
    OVERRIDE(CONFIG_ETHERNET_ITEM_MAC, ethernet_fields),
    OVERRIDE(CONFIG_ETHERNET_ITEM_IP, ethernet_fields),
    OVERRIDE(CONFIG_ETHERNET_ITEM_SUBNET, ethernet_fields),
    OVERRIDE(CONFIG_ETHERNET_ITEM_GATEWAY, ethernet_fields),
    OVERRIDE(CONFIG_ETHERNET_ITEM_DHCP, ethernet_dhcp),
    OVERRIDE(CONFIG_ETHERNET_ITEM_TEST, ethernet_test),
};

/* ======================================================================== */
/*  Z80 APPLICARD                                                           */
/* ======================================================================== */
HELP(applicard,
    "The Z80 Applicard is a PCPI Appli-Card compatible coprocessor in slot 5: a virtual Z80 running",
    "near 80 MHz with 2 MB of banked RAM. The boot ROM is built into the firmware.",
    "Boot PCPI CP/M from the Disk II to use it. Physical slot 5 must be empty.");

HELP(applicard_resource,
    "Resource usage sets how much Appletini CPU time the Z80 may claim per pass.",
    "Standard is right for everyday use. Maximum speeds up long computations (compilers,",
    "number crunching) by roughly 10%, at the cost of slightly less responsive menus while",
    "the Z80 is working flat out. Disk and console speed are unaffected.");

static const help_override_t applicard_overrides[] = {
    OVERRIDE(1, applicard_resource),
};

/* ======================================================================== */
/*  CLOCK   (config_menu.c inserts a live link-state line)                  */
/* ======================================================================== */
HELP(clock,
    "Clock exposes the PCF8563 real-time clock as a \"no-slot\" clock for Apple software.",
    "\"Read RTC\" loads hardware time into the fields; \"Write RTC\" stores the edited date and time.",
    "The clock is battery-backed and keeps time when the Appletini ONE is powered off.",
    "Check the CR2032 coin battery voltage regularly, and replace it when it drops below 2.5V.");

/* ======================================================================== */
/*  RAM                                                                     */
/* ======================================================================== */
HELP(ram,
    "Memory the Appletini provides to the Apple IIe. Change it from BOOT mode before software runs.");

HELP(ram_provide,
    "Appletini serves 64K auxiliary memory (80-column text, double hi-res, ProDOS /RAM) plus a",
    "RamWorks III compatible 8MB expansion (128 banks; software sizes it by probing).",
    "Change this only from BOOT mode (press 'A' during boot); USB-owned mode keeps it locked.",
    "The boot ROM probes the aux slot at every boot: if a physical extended 80-column card is found,",
    "Appletini RAM stays off automatically and this switch is ignored.");

static const help_override_t ram_overrides[] = {
    OVERRIDE(0, ram_provide),
};

/* ======================================================================== */
/*  USB                                                                     */
/* ======================================================================== */
HELP(usb,
    "USB controls the USB0 device presented to the host computer.",
    "By default USB0 is detached. SuperDuperDisplay is persistent; SD Card Remote Mounting is a modal",
    "maintenance mode that should be exited after ejecting the disk on the host.");

HELP(usb_sdd,
    "SDD stream: sends every Apple bus cycle over USB0 to SuperDuperDisplay running on a PC, which",
    "regenerates video and audio there.",
    "USB0 enumerates as a SuperDuperDisplay device while streaming. The SD-card remote mount stays",
    "detached until you explicitly start it from this tab.");

HELP(usb_sd_remote,
    "SD Card Remote Mounting exposes the card's SD filesystem to the host over USB0.",
    "This is modal because desktop operating systems issue heavy command bursts. Appletini services",
    "only the bridge and exit controls until you eject on the host and leave this mode.",
    "Software running on the Z80 Applicard pauses during the mount and resumes when you exit.");

static const help_override_t usb_overrides[] = {
    OVERRIDE(0, usb_sd_remote),
    OVERRIDE(1, usb_sdd),
};

/* ======================================================================== */
/*  ABOUT                                                                   */
/* ======================================================================== */
HELP(about,
    "The firmware is open-source and available on GitHub. We only stand on the shoulders of giants.",
    "                                                            Rikkles && KKR75 - Yarze, Lebanon");

/* ======================================================================== */
/*  MASTER TABLE -- one row per tab.                                        */
/*  Use TAB(...) for a plain tab, TAB_WITH_OVERRIDES(...) if it has         */
/*  per-item help. The order here does not matter; lookup is by tab id.     */
/* ======================================================================== */
static const help_tab_t k_help_tabs[] = {
    TAB(CONFIG_TAB_PROFILES,      profiles),
    TAB(CONFIG_TAB_BOOT_SETTINGS, boot_settings),
    TAB_WITH_OVERRIDES(CONFIG_TAB_VIDEO, video, video_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_SMARTPORT, smartport, smartport_overrides),
    TAB(CONFIG_TAB_DISK2,         disk2),
    TAB(CONFIG_TAB_MOUSE,         mouse),
    TAB_WITH_OVERRIDES(CONFIG_TAB_MOCKINGBOARD, phasor, phasor_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_ETHERNET, ethernet, ethernet_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_APPLICARD, applicard, applicard_overrides),
    TAB(CONFIG_TAB_CLOCK,         clock),
    TAB_WITH_OVERRIDES(CONFIG_TAB_RAM, ram, ram_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_USB, usb, usb_overrides),
    TAB(CONFIG_TAB_ABOUT,         about),
};

/* ------------------------------------------------------------------------ */

config_menu_help_block_t config_menu_help_resolve(uint32_t tab, uint32_t item)
{
    config_menu_help_block_t block = { NULL, 0U };

    for (uint32_t t = 0U; t < HELP_COUNT(k_help_tabs); ++t) {
        const help_tab_t *entry = &k_help_tabs[t];

        if (entry->tab != tab) {
            continue;
        }

        /* Per-item override wins over the tab default. */
        for (uint32_t o = 0U; o < entry->override_count; ++o) {
            if (entry->overrides[o].item == item) {
                block.lines = entry->overrides[o].lines;
                block.count = entry->overrides[o].count;
                return block;
            }
        }

        block.lines = entry->default_lines;
        block.count = entry->default_count;
        return block;
    }

    return block;
}
