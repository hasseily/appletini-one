set xpr_path "project/appletini_yarz.xpr"

proc require_equal {actual expected message} {
    if {$actual ne $expected} {
        error "$message: expected '$expected', got '$actual'"
    }
}

open_project $xpr_path
set synth_run [get_runs synth_1]
set impl_run [get_runs impl_1]

set_property -dict \
    [list {STEPS.ROUTE_DESIGN.ARGS.MORE OPTIONS} {-tns_cleanup}] $impl_run
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED true $impl_run
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE Explore $impl_run

require_equal \
    [string trim [get_property {STEPS.ROUTE_DESIGN.ARGS.MORE OPTIONS} $impl_run]] \
    -tns_cleanup "Route extra options"
require_equal \
    [get_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED $impl_run] \
    1 "Post-route physical optimization enable"
require_equal \
    [get_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE $impl_run] \
    Explore "Post-route physical optimization directive"

set global_retiming \
    [get_property STEPS.SYNTH_DESIGN.ARGS.GLOBAL_RETIMING $synth_run]
if {$global_retiming ni {auto off on}} {
    error "Unexpected global retiming value '$global_retiming'."
}

close_project
puts "PASS: timing run properties"
