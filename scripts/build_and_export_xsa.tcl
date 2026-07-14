set xpr_path "project/appletini_yarz.xpr"
set xsa_out  "project/appletini_yarz_top.xsa"

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
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1

# Export XSA including bitstream
# write_hw_platform is the modern flow; include_bit ensures bit is packaged.
puts "Exporting hardware platform to: $xsa_out"
write_hw_platform -fixed -include_bit -force -file $xsa_out

close_project
puts "Done."
