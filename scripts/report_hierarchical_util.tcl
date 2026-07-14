# Run from the project root via:
#   vivado -mode batch -nojournal -nolog -source scripts/report_hierarchical_util.tcl
#
# Opens the routed implementation checkpoint and writes a
# hierarchical utilization report grouped by module instance.

set dcp "project/appletini_yarz.runs/impl_1/appletini_yarz_top_routed.dcp"
set out "project/appletini_yarz.runs/impl_1/appletini_yarz_top_utilization_hier.rpt"

open_checkpoint $dcp
report_utilization -hierarchical -hierarchical_depth 4 -file $out
close_design
exit
