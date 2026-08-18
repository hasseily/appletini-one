# Synthesize the complete Appletini top after scripts/create_project.tcl.
# Use the project run so Vivado builds generated Zynq block-design IP before
# it checks the handwritten top-level RTL.

set script_dir [file dirname [file normalize [info script]]]
set repo_root [file normalize [file join $script_dir ".."]]
set project_path [file join $repo_root project appletini_yarz.xpr]

open_project $project_path
generate_target all [get_files zynq_ps_bd.bd]
update_compile_order -fileset sources_1
reset_run synth_1
launch_runs synth_1 -jobs 8
wait_on_run synth_1

set synth_status [get_property STATUS [get_runs synth_1]]
if {$synth_status ne "synth_design Complete!"} {
    error "ONEE top synthesis failed: $synth_status"
}

puts "ONEE TOP SYNTHESIS PASS"
close_project
