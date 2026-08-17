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

HELP(profiles_choose,
    "Choose profile opens the carousel rooted at 0:/profiles.",
    "Use Left/Right to browse, Enter to open a folder or load a profile, and Back to move up or close.",
    "Loading a profile replaces the working configuration and immediately applies its saved settings.");

HELP(profiles_save_current,
    "Save to current profile writes the current menu configuration into the selected profile.",
    "It is available after Choose profile or Save As and overwrites that profile's saved settings.",
    "The profile name and thumbnail are left unchanged.");

HELP(profiles_save_as,
    "Save As creates a profile in the carousel's current folder from the working configuration.",
    "Enter a new name with the Apple keyboard in BOOT mode or the on-screen keyboard in USB mode.",
    "The new profile becomes current; use Set image separately if you want a thumbnail.");

HELP(profiles_rename,
    "Rename profile changes the selected profile's folder name without changing its settings or thumbnail.",
    "It is available only after a profile is selected. Existing or invalid names are rejected.");

HELP(profiles_set_image,
    "Set image selects a PNG for the current profile and stores it as a normalized 560x384 thumbnail.",
    "The source PNG is not changed. This action is available only after a profile is selected.");

static const help_override_t profiles_overrides[] = {
    OVERRIDE(0U, profiles_choose),
    OVERRIDE(1U, profiles_save_current),
    OVERRIDE(2U, profiles_save_as),
    OVERRIDE(3U, profiles_rename),
    OVERRIDE(4U, profiles_set_image),
};

/* ======================================================================== */
/*  BOOT SETTINGS                                                           */
/* ======================================================================== */
HELP(boot_settings,
    "The wait sets how long the 'A' prompt shows before boot.",
    "3 or 5 seconds boot after the wait. Unlimited waits for 'A' or Esc. Always show menu opens the menu now.",
    "Press 'A' at the prompt for BOOT mode; the Apple keyboard drives the menu.",
    "In normal use the USB device's MENU key opens USB mode, and the USB keys drive the menu.",
    "Leave the Apple keyboard alone in USB mode: it reaches the running program.",
    "In USB bindings, Up/Down move within a column, Left/Right move columns, and Enter captures a new input.",
    "ONE//e fixes Reset, Open Apple, Closed Apple, and Pause/Break. Other USB bindings stay editable.",
    "Pause/Break and a long press of the saved OK binding both open or close the ONE//e menu.",
    "TW keys change TransWarp speed any time: toggle 1 MHz, step the preset, toggle the 0.05 MHz slug.");

HELP(boot_timeout,
    "How long the boot 'A' prompt shows before the machine boots on its own.",
    "3 or 5 seconds boot after the wait. Unlimited waits until you press 'A' (menu) or Esc (boot).",
    "Always show menu skips the prompt and opens this menu on every power-on.");

HELP(boot_device,
    "Which drive the Appletini boots from: the SmartPort drives or the Disk II drives.",
    "SmartPort boots the images on the SmartPort tab; Disk II boots the floppy images on the Disk II tab.",
    "ONE//e always has virtual Disk II. It keeps this choice even when physical Slot 6 is off.",
    "An Apple host falls back to SmartPort when Disk II is selected but physical Slot 6 is off.");

HELP(boot_onee,
    "Runs the built-in Enhanced Apple //e on Appletini's soft 65C02 without an Apple host.",
    "The selection survives a card power cycle and starts only after the connector is quiet.",
    "Any Apple-bus activity stops ONE//e and saves it OFF.",
    "After the connector is quiet, select this item again to save it ON.");

HELP(boot_onee_standard,
    "Chooses cadence for the built-in ONE//e only: NTSC has 262 lines and 130 fabric clocks per cycle.",
    "PAL has 312 lines and 131 fabric clocks per cycle. Both begin vertical blank at scanner line 192.",
    "The row shows the saved target. A live change stays pending until the next virtual reset or restart.",
    "This row appears only while ONE//e is active. A physical Apple's video standard remains automatic.");

HELP(boot_bind_reset,
    "Restores every USB menu binding below to its factory default.",
    "Bindings are editable only while the menu was opened from the Apple boot prompt ('A' + BOOT mode).");

HELP(boot_bind_nav,
    "Moves the menu selection: Up/Down within a column, Left/Right across columns.",
    "Press Enter on the row, then press the USB key or gamepad input you want to bind.");

HELP(boot_bind_tabs,
    "Cycles the menu tabs up or down from anywhere in the menu.",
    "Press Enter on the row, then press the USB key or gamepad input you want to bind.");

HELP(boot_bind_ok_back,
    "OK activates the selected item; BACK leaves a browser or closes the menu.",
    "Press Enter on the row, then press the USB key or gamepad input you want to bind.");

HELP(boot_bind_prtscr,
    "Screenshot keys, active at all times, not just in the menu.",
    "PRTSCR A2 saves the Apple's native video frame; PRTSCR 1080P saves the full 1080p HDMI output.",
    "Screenshots land on the SD card.");

HELP(boot_bind_tw_speed,
    "TransWarp speed keys, active at all times: toggle between full speed and 1 MHz,",
    "or step the speed preset up and down. They work only while the TransWarp is enabled.");

HELP(boot_bind_tw_slug,
    "Toggles the 0.05 MHz slug speed for debugging, active at all times.",
    "The key works only when the slug debug key is armed on the TransWarp tab.");

/* Item order outside ONE//e: 0 boot wait, 1 boot device, 2 ONE//e, 3 reset
 * bindings, then the bindings in k_boot_usb_binding_action_order: 4-7 nav,
 * 8-9 tabs, 10 OK, 11 BACK, 12-13 screenshots, 14-16 TW speed, 17 TW slug.
 * While ONE//e is active, item 3 becomes the video-standard row. */
static const help_override_t boot_settings_overrides[] = {
    OVERRIDE(0,  boot_timeout),
    OVERRIDE(1,  boot_device),
    OVERRIDE(2,  boot_onee),
    OVERRIDE(3,  boot_bind_reset),
    OVERRIDE(4,  boot_bind_nav),
    OVERRIDE(5,  boot_bind_nav),
    OVERRIDE(6,  boot_bind_nav),
    OVERRIDE(7,  boot_bind_nav),
    OVERRIDE(8,  boot_bind_tabs),
    OVERRIDE(9,  boot_bind_tabs),
    OVERRIDE(10, boot_bind_ok_back),
    OVERRIDE(11, boot_bind_ok_back),
    OVERRIDE(12, boot_bind_prtscr),
    OVERRIDE(13, boot_bind_prtscr),
    OVERRIDE(14, boot_bind_tw_speed),
    OVERRIDE(15, boot_bind_tw_speed),
    OVERRIDE(16, boot_bind_tw_speed),
    OVERRIDE(17, boot_bind_tw_slug),
    OVERRIDE(CONFIG_MENU_BOOT_ONEE_STANDARD_HELP_ITEM, boot_onee_standard),
};

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

HELP(video_video7_mix,
    "COL140M is also the Video-7 mode called MIX. It mixes color and monochrome within one DHGR scanline.",
    "It is present in the Video-7 and Chat Mauve RGB cards. The most famous image in this mode is the",
    "elephant image. The Appletini demo disk has such an image as well.",
    "When enabled, Video-7 state 10 selects MIX through the $C05E/$C05F soft-switch sequence.",
    "The byte high bits select color or monochrome at each four-dot color boundary.");

HELP(video_scanlines,
    "Scanlines blank replicated output rows after scaling. It is a naive but effective effect.",
    "There is no performance impact, and it is purely a matter of preference.");

HELP(video_ghosting,
    "Phosphor ghosting retains bright pixels from earlier displayed frames and decays them over time.",
    "Light, Medium, and Strong increase persistence, so motion and flashes leave longer trails.",
    "Ghosting is expensive, so avoid mixing it with borders, full screen bezels and debug.");

HELP(video_blur,
    "Phosphor blur softens the Apple video like a CRT spot, bleeding each pixel into its neighbors.",
    "Light softens horizontally. Medium adds vertical bleed across scanlines. Strong widens further.",
    "Blur combines with glow and ghosting for a full CRT look.",
    "Blur+glow are expensive, so avoid mixing them with borders, full screen bezels and debug.");

HELP(video_glow,
    "Phosphor glow adds a halo of light around bright pixels without softening the image itself.",
    "The halo is additive and saturates toward white, like a CRT bloom. Strengths set its intensity.",
    "Glow is independent of blur: sharp with glow, soft with glow, or both work together.",
    "Blur+glow are expensive, so avoid mixing them with borders, full screen bezels and debug.");

HELP(video_format_badge,
    "Show video mode labels the current Apple format (HGR, DHGR, SHR4, 3200...) in a corner.",
    "SHR submodes are read from the in-band SDD control bytes; legacy modes from the soft switches.",
    "Interlaced legacy variants report their base mode until legacy interlace support lands.");

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
    "Debugging has a mild performance impact. Let us know if you want additional debug information.");

static const help_override_t video_overrides[] = {
    OVERRIDE(CONFIG_VIDEO_ITEM_OUTPUT, video_output),
    OVERRIDE(CONFIG_VIDEO_ITEM_VARIANT, video_variant),
    OVERRIDE(CONFIG_VIDEO_ITEM_SCANLINES, video_scanlines),
    OVERRIDE(CONFIG_VIDEO_ITEM_BLUR, video_blur),
    OVERRIDE(CONFIG_VIDEO_ITEM_GLOW, video_glow),
    OVERRIDE(CONFIG_VIDEO_ITEM_GHOSTING, video_ghosting),
    OVERRIDE(CONFIG_VIDEO_ITEM_BORDER, video_border),
    OVERRIDE(CONFIG_VIDEO_ITEM_VIDEO7, video_video7),
    OVERRIDE(CONFIG_VIDEO_ITEM_COL140M, video_video7_mix),
    OVERRIDE(CONFIG_VIDEO_ITEM_BORDER_COLOR, video_border_color),
    OVERRIDE(CONFIG_VIDEO_ITEM_BORDER_FLOOD, video_border_outside),
    OVERRIDE(CONFIG_VIDEO_ITEM_ROM, video_rom),
    OVERRIDE(CONFIG_VIDEO_ITEM_SHOW_BEZEL, video_show_bezel),
    OVERRIDE(CONFIG_VIDEO_ITEM_BEZEL, video_bezel),
    OVERRIDE(CONFIG_VIDEO_ITEM_DEBUG, video_debug),
    OVERRIDE(CONFIG_VIDEO_ITEM_BADGE, video_format_badge),
};

/* ======================================================================== */
/*  SMARTPORT                                                               */
/* ======================================================================== */
HELP(smartport,
    "SmartPort presents up to eight block devices as ProDOS/SOS-style mass-storage units.",
    "Each SP row selects, replaces, or clears an image on the SD card; duplicate images are blocked.",
    "Supported: HDV, 2MG, 2IMG, and PO images of any size.",
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
    "Supported: WOZ, NIB, DSK, DO, PO, 2MG, and 2IMG (16-sector only).");

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
    "When disabled, all network configuration and actions are hidden and DHCP cannot run.",
    "Enable the card to expose its address, saved configuration, DHCP, and link-test controls.");

HELP(ethernet_config,
    "When Enable in Slot 1 is on, configure at boot writes static fields or runs DHCP according to",
    "Address Mode. DHCP runs in the background after startup and negotiates a fresh lease instead of",
    "replaying the old address. It may take up to 20s for DHCP to complete, so be patient before testing",
    "network software after a boot. It is always faster to use a static address.");

HELP(ethernet_fields,
    "Enter moves to the next byte or octet; Left/Right decrements or increments the selected value.",
    "MAC is saved as six hexadecimal bytes; IP, subnet, and gateway are saved as dotted decimal.");

HELP(ethernet_dhcp,
    "DHCP uses the configured MAC address to request IP, subnet, and gateway from the local network.",
    "A successful lease enables boot configuration; later boots request a fresh lease.");

HELP(ethernet_test,
    "Test link reads the W5100S identity/version, PHY link status, and current IP from the card.");

HELP(ethernet_ftp_sd,
    "Starts anonymous read/write FTP access to the SD card on TCP port 21.",
    "Only clients in the configured local subnet can connect; active FTP mode is not supported.",
    "FTP owns Ethernet and the SD card until Enter, Escape, Back, or Menu stops sharing.",
    "Use this only on a trusted LAN: FTP sends file data without encryption.");

static const help_override_t ethernet_overrides[] = {
    OVERRIDE(CONFIG_ETHERNET_ITEM_CONFIG_ENABLED, ethernet_config),
    OVERRIDE(CONFIG_ETHERNET_ITEM_MAC, ethernet_fields),
    OVERRIDE(CONFIG_ETHERNET_ITEM_IP, ethernet_fields),
    OVERRIDE(CONFIG_ETHERNET_ITEM_SUBNET, ethernet_fields),
    OVERRIDE(CONFIG_ETHERNET_ITEM_GATEWAY, ethernet_fields),
    OVERRIDE(CONFIG_ETHERNET_ITEM_DHCP, ethernet_dhcp),
    OVERRIDE(CONFIG_ETHERNET_ITEM_TEST, ethernet_test),
    OVERRIDE(CONFIG_ETHERNET_ITEM_FTP_SD, ethernet_ftp_sd),
};

/* ======================================================================== */
/*  SLOT 5 PROCESSOR                                                        */
/* ======================================================================== */
HELP(applicard,
    "Choose one virtual processor card for slot 5. The Z80 option is PCPI Appli-Card compatible",
    "with 2 MB of banked RAM. The ALF option emulates a fully expanded 640 KB AD8088 Plus",
    "and its shared Apple-memory window. Physical slot 5 must be empty.");

HELP(applicard_processor,
    "Z80 Appli-Card runs PCPI CP/M with the firmware's built-in boot ROM.",
    "ALF AD8088 Plus provides an 8088 and 640 KB RAM, including the AD128K-compatible range.",
    "Changing the selection resets the selected virtual processor. Turn virtual TransWarp off",
    "while using AD8088 because both processors need to master the Apple bus.");

HELP(applicard_resource,
    "Resource usage sets how much Appletini CPU time the selected processor may claim per pass.",
    "Standard is right for everyday use. Maximum improves sustained long computations (compilers,",
    "number crunching), at the cost of slightly less responsive menus while",
    "the coprocessor is working flat out. Disk and console speed are unaffected.");

static const help_override_t applicard_overrides[] = {
    OVERRIDE(1, applicard_processor),
    OVERRIDE(2, applicard_resource),
};

/* ======================================================================== */
/*  TRANSWARP                                                               */
/* ======================================================================== */

HELP(transwarp,
    "A virtual TransWarp accelerator. Never use with a physical accelerator installed.",
    "When accelerating, the machine becomes an Enhanced //e with the full 128K. RamWorks can also",
    "be enabled for an additional 8MB that runs at accelerator speed.",
    "Turn acceleration on or off from BOOT mode only (press 'A' during boot); the speed",
    "preset can be changed at any time, even mid-session. Boot settings can bind USB keys",
    "for live speed control: 1 MHz toggle, speed up/down, and the 0.05 MHz slug toggle.");

HELP(transwarp_speed,
    "Warp runs the 65C02 as fast as the fabric allows (~ 35 MHz). 3.6 MHz matches the real TransWarp.",
    "1 MHz cycle-exact locks every core cycle to the Apple bus clock and passes cycle-counting",
    "speed detectors and vapor lock, but disable acceleration for really precise demos.",
    "Most I/O and video writes always run at 1 MHz bus speed, whatever the setting.");

HELP(transwarp_ignore_c074,
    "$C074 is the TransWarp software speed switch: $00 selects fast, $01 selects 1 MHz,",
    "and $03 disables acceleration until reset. When checked, vTW ignores every write to",
    "$C074 and keeps the speed chosen here or by USB. This can break copy protection, disk",
    "access, sound, serial I/O, and other code that uses $C074 to protect timed operations.");

HELP(transwarp_disable_disk2_accel,
    "Disables vTW's private fast-read path for the virtual Disk II card. Every Disk II access",
    "then uses the original physical 1 MHz path. Use this if a WOZ or copy-protected disk",
    "fails with Disk II acceleration. Reads will be slower; writes already use the 1 MHz path.");

HELP(transwarp_slug,
    "Arms the 0.05 MHz 'slug' USB speed key for extremely slow debugging. Off by default:",
    "the slug key is ignored until armed here, so an accidental press mid-session can't drop",
    "the machine to a near-halt without warning. When armed, the bound slug key toggles 0.05 MHz",
    "against the configured speed, and a notice appears at the top of the screen on each toggle.",
    "Disarming this while slugged immediately restores the configured speed. The setting persists.");

HELP(transwarp_slowdown_floatbus,
    "Slows the 65C02 to 1 MHz after floating-bus/video I/O in $C030-$C05F or VBL status $C019.",
    "This covers the speaker, undriven motherboard I/O, graphics switches and annunciators.",
    "Use it for correct speaker pitch and software that cycle-counts against the video scanner.",
    "Floating-bus read data is always emulated; this option controls only the slowdown window.");

HELP(transwarp_slowdown_paddle,
    "Slows down the 65C02 to 1 MHz for a short window after getting paddle or joystick input.",
    "The slowdown window is the same as the other slowdown regions.");

HELP(transwarp_slowdown_slots,
    "Enable slowdown for the slots where you have physical cards that are sensitive to timings or",
    "cycle-counting detection. This includes disk controllers, mouse cards, music cards and other",
    "peripherals that expect the Apple bus to run at 1 MHz.",
    "Per-slot slowdown mirrors the real TransWarp's DIP block 2: after the core touches an enabled",
    "timing-sensitive region, it drops to 1 MHz for the slowdown window, then resumes full speed.");

HELP(transwarp_slowdown_window,
    "Controls the duration of the slowdown window for all slowdown regions.",
    "The window is how long (in cycles) each touch stays at 1 MHz. The default of 512 cycles",
    "should be long enough for most software; pick 16k or 32k when a very fast core needs to",
    "stay locked through longer stretches, like a beam-synced effect or a long device loop.");

static const help_override_t transwarp_overrides[] = {
    OVERRIDE(1, transwarp_speed),
    OVERRIDE(2, transwarp_ignore_c074),
    OVERRIDE(3, transwarp_disable_disk2_accel),
    OVERRIDE(4, transwarp_slowdown_floatbus),
    OVERRIDE(5, transwarp_slowdown_paddle),
    OVERRIDE(6, transwarp_slug),
    OVERRIDE(7, transwarp_slowdown_slots),
    OVERRIDE(8, transwarp_slowdown_slots),
    OVERRIDE(9, transwarp_slowdown_slots),
    OVERRIDE(10, transwarp_slowdown_slots),
    OVERRIDE(11, transwarp_slowdown_slots),
    OVERRIDE(12, transwarp_slowdown_slots),
    OVERRIDE(13, transwarp_slowdown_slots),
    OVERRIDE(14, transwarp_slowdown_window),
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
    "Appletini serves 64K auxiliary memory plus a RamWorks III compatible 8MB expansion.",
    "Change this only from BOOT mode (press 'A' during boot);",
    "The boot ROM probes the aux slot at every boot: if a physical extended 80-column card is found,",
    "Appletini RAM stays off automatically and this switch is ignored.",
    "With TransWarp acceleration on, 128K is built into the accelerator and the 8MB RamWorks",
    "is served from PSRAM at accelerator speed (a physical aux card disables both, as always).");

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
    "SDD stream sends every Apple bus cycle over USB0 to SuperDuperDisplay running on a PC, which",
    "regenerates video and audio there. SDD is open source software on GitHub.",
    "SD Card Remote Mounting is not available while SDD is active.");

HELP(usb_sd_remote,
    "SD Card Remote Mounting exposes the card's SD filesystem to the host over USB0.",
    "This is modal because desktop operating systems issue heavy command bursts. Appletini services",
    "only the bridge and exit controls until you eject on the host and leave this mode.",
    "Software running on the slot 5 processor pauses during the mount and resumes when you exit.",
    "SD Card Remote Mounting is not available while SDD is active.");

static const help_override_t usb_overrides[] = {
    OVERRIDE(0, usb_sd_remote),
    OVERRIDE(1, usb_sdd),
};

/* ======================================================================== */
/*  PRINTING                                                                */
/* ======================================================================== */
HELP(printing,
    "The virtual Super Serial Card prints as an ImageWriter II. It shares slot 1 with the",
    "Uthernet II. Print from the Apple with PR#1 (or select an SSC in slot 1 in your software).",
    "Each printed page becomes a PNG file in 0:/printouts on the SD card.");

HELP(printing_browse,
    "Browse the saved printouts with a preview. ENTER renames the selected printout,",
    "SPACE deletes it after a confirmation. A print job closes a few seconds after the",
    "Apple stops sending data; the last partial page is saved at that point.");

static const help_override_t printing_overrides[] = {
    OVERRIDE(1, printing_browse),
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
    TAB_WITH_OVERRIDES(CONFIG_TAB_PROFILES, profiles, profiles_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_BOOT_SETTINGS, boot_settings,
                       boot_settings_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_VIDEO, video, video_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_SMARTPORT, smartport, smartport_overrides),
    TAB(CONFIG_TAB_DISK2,         disk2),
    TAB(CONFIG_TAB_MOUSE,         mouse),
    TAB_WITH_OVERRIDES(CONFIG_TAB_MOCKINGBOARD, phasor, phasor_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_ETHERNET, ethernet, ethernet_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_APPLICARD, applicard, applicard_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_TRANSWARP, transwarp, transwarp_overrides),
    TAB(CONFIG_TAB_CLOCK,         clock),
    TAB_WITH_OVERRIDES(CONFIG_TAB_RAM, ram, ram_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_USB, usb, usb_overrides),
    TAB_WITH_OVERRIDES(CONFIG_TAB_PRINTING, printing, printing_overrides),
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
