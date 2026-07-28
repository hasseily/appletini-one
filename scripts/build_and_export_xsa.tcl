set xpr_path "project/appletini_yarz.xpr"
set xsa_out  "project/appletini_yarz_top.xsa"
set incremental_ref       ".vivado_cache/appletini_yarz_top_known_good.dcp"
set incremental_candidate ".vivado_cache/appletini_yarz_top_candidate.dcp"
set force_full_build [expr {
    [info exists ::env(APPLETINI_FULL_BUILD)] &&
    $::env(APPLETINI_FULL_BUILD) ne "" &&
    $::env(APPLETINI_FULL_BUILD) ne "0"
}]

# Child OOC/IP synthesis runs inherit the environment from this batch process.
# Keep them on Vivado's built-in Tcl Store so user-installed Tcl apps cannot
# break generated run scripts.
proc configure_batch_tclapp_repo {} {
    set candidates {}

    if {[info exists ::env(XILINX_VIVADO)]} {
        lappend candidates [file join $::env(XILINX_VIVADO) data XilinxTclStore]
    }

    if {![catch {version -short} vivado_version]} {
        lappend candidates [file join C:/ Xilinx $vivado_version Vivado data XilinxTclStore]
    }

    foreach candidate $candidates {
        set repo [file normalize $candidate]
        if {[file isdirectory $repo]} {
            set ::env(XILINX_TCLAPP_REPO) $repo
            set ::env(XILINX_LOCAL_USER_DATA) "NO"
            puts "Using Vivado Tcl Store for batch runs: $repo"
            return
        }
    }

    puts "WARNING: Vivado Tcl Store not found; batch runs may load user Tcl apps."
}

configure_batch_tclapp_repo

puts "Opening project: $xpr_path"
open_project $xpr_path

# Always synthesize from current RTL. Generated partitions in an incremental
# checkpoint may not match the source being validated.
if {[llength [get_runs synth_1 -quiet]]} {
    catch {set_property AUTO_INCREMENTAL_CHECKPOINT 0 [get_runs synth_1]}
    catch {set_property INCREMENTAL_CHECKPOINT "" [get_runs synth_1]}
}

# Reuse placement/routing from the last timing-clean implementation. Vivado
# matches unchanged cells and implements unmatched logic normally, so source
# changes can still proceed without treating the reference as a fixed netlist.
if {[llength [get_runs impl_1 -quiet]]} {
    catch {set_property AUTO_INCREMENTAL_CHECKPOINT 0 [get_runs impl_1]}
    catch {set_property INCREMENTAL_CHECKPOINT "" [get_runs impl_1]}
    if {$force_full_build} {
        puts "APPLETINI_FULL_BUILD requested; running without an incremental reference."
    } elseif {[file isfile $incremental_ref]} {
        set incremental_ref_abs [file normalize $incremental_ref]
        set_property INCREMENTAL_CHECKPOINT $incremental_ref_abs [get_runs impl_1]
        puts "Using incremental implementation reference: $incremental_ref_abs"
    } else {
        puts "No incremental implementation reference found; running a full implementation."
    }
}

# If you need a specific top, you can uncomment and set it explicitly:
# set_property top appletini_yarz_top [current_fileset]

# check for syntax errors
puts "Checking syntax..."
check_syntax

# Run synthesis + implementation through write_bitstream
puts "Launching synthesis..."
reset_run synth_1 -quiet
launch_runs synth_1 -jobs 8
wait_on_run synth_1

puts "Launching implementation to write_bitstream..."
reset_run impl_1 -quiet
# Post-route physical optimization. This design sits at the edge of the
# 133 MHz fabric-clock timing budget, so a from-scratch (non-incremental)
# placement can leave sub-100 ps setup violations on pre-existing
# carry-chain paths (the audio mixer, etc.). Post-route phys_opt closes
# these deterministically; it is a no-op once timing is already met.
catch {set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED true [get_runs impl_1]}
catch {set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE Explore [get_runs impl_1]}
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1

# Save a timing-clean final design as a candidate. Timing closure alone does
# not prove that the image boots on hardware, so only the explicit promotion
# script may replace the known-good incremental reference.
open_run impl_1
set worst_setup_path [get_timing_paths -quiet -delay_type max -max_paths 1]
set worst_hold_path  [get_timing_paths -quiet -delay_type min -max_paths 1]
if {[llength $worst_setup_path] == 0 || [llength $worst_hold_path] == 0} {
    error "Unable to verify setup/hold timing before checkpoint promotion."
}
set worst_setup_slack [get_property SLACK $worst_setup_path]
set worst_hold_slack  [get_property SLACK $worst_hold_path]
puts "Final implementation slack: setup=$worst_setup_slack ns hold=$worst_hold_slack ns"
if {$worst_setup_slack < 0.0 || $worst_hold_slack < 0.0} {
    error "Timing failed; refusing to export hardware or replace the incremental reference."
}
file mkdir [file dirname $incremental_candidate]
write_checkpoint -force $incremental_candidate
puts "Saved timing-clean candidate checkpoint: [file normalize $incremental_candidate]"
puts "After hardware validation, run scripts/promote_timing_candidate.tcl to lock this placement."

# Export XSA including bitstream
# write_hw_platform is the modern flow; include_bit ensures bit is packaged.
puts "Exporting hardware platform to: $xsa_out"
write_hw_platform -fixed -include_bit -force -file $xsa_out

close_project
puts "Done."
