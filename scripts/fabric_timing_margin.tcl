# One temporary optimization margin for the build process and its run hooks.
# Final signoff always restores zero user uncertainty and nominal board limits.
namespace eval appletini_timing_margin {}

proc appletini_timing_margin::value {} {
    set margin 0.200
    if {[info exists ::env(APPLETINI_IMPLEMENTATION_SETUP_MARGIN_NS)]} {
        set margin $::env(APPLETINI_IMPLEMENTATION_SETUP_MARGIN_NS)
    }
    if {![string is double -strict $margin] ||
        [catch {expr {double($margin) >= 0.200 && double($margin) <= 1.000}} valid] ||
        !$valid} {
        error "APPLETINI_IMPLEMENTATION_SETUP_MARGIN_NS must be finite and between 0.200 and 1.000 ns."
    }
    set canonical [format %.3f $margin]
    if {abs(double($margin) - double($canonical)) > 0.000000001} {
        error "APPLETINI_IMPLEMENTATION_SETUP_MARGIN_NS must use 0.001 ns steps."
    }
    return $canonical
}
