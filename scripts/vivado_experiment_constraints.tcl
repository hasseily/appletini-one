# Optional implementation aids only. The experiment driver restores nominal
# timing before reports; all board and CDC constraints remain in the XDC.
set experiment_unpin 1
if {[info exists ::env(APPLETINI_EXPERIMENT_UNPIN)]} {
    set experiment_unpin $::env(APPLETINI_EXPERIMENT_UNPIN)
}
if {$experiment_unpin} {
    foreach name {apple_data_enable_lut apple_addr_enable_lut} {
        # Vivado keeps a generate-block prefix in a primitive's leaf name
        # (gen_fast_addr_enable.apple_addr_enable_lut).
        set cells [get_cells -hier -filter "NAME =~ *$name"]
        if {[llength $cells] != 1} {error "Expected exactly one $name cell"}
        reset_property LOC $cells
        reset_property BEL $cells
        if {[get_property LOC $cells] ne "" || [get_property BEL $cells] ne ""} {
            error "Could not clear placement hint on $cells"
        }
        puts "Removed optional LOC/BEL from $cells; retained its logic and DONT_TOUCH."
    }
}
set experiment_margin 0.0
if {[info exists ::env(APPLETINI_EXPERIMENT_MARGIN_NS)]} {
    set experiment_margin $::env(APPLETINI_EXPERIMENT_MARGIN_NS)
}
if {![string is double -strict $experiment_margin] ||
    ($experiment_margin != 0.0 && $experiment_margin != 0.2)} {
    error "APPLETINI_EXPERIMENT_MARGIN_NS must be 0 or 0.2."
}
if {$experiment_margin > 0.0} {
    # Reuse the checked margin hooks: tighten fabric setup and the Apple
    # direction outputs, then restore their exact nominal limits for reports.
    source [file join [file dirname [info script]] apply_fabric_timing_margin.tcl]
}
