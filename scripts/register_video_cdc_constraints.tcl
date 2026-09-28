# Use from project creation and both build drivers, including saved projects.
# Keep validation in normal Tcl; XDC does not support if/error commands.
set video_cdc_path [file normalize [file join [file dirname [info script]] \
    .. hdl constraints video_cdc_impl.xdc]]
if {![file isfile $video_cdc_path]} {
    error "Missing implementation video CDC constraints: $video_cdc_path"
}
set video_cdc_file [get_files -quiet -of_objects [get_filesets constrs_1] \
    $video_cdc_path]
if {[llength $video_cdc_file] == 0} {
    add_files -fileset constrs_1 -norecurse $video_cdc_path
    set video_cdc_file [get_files -quiet -of_objects [get_filesets constrs_1] \
        $video_cdc_path]
}
if {[llength $video_cdc_file] != 1} {
    error "Expected exactly one implementation video CDC constraint file"
}
set_property -dict [list USED_IN_SYNTHESIS false \
    USED_IN_IMPLEMENTATION true PROCESSING_ORDER LATE] $video_cdc_file
if {[get_property USED_IN_SYNTHESIS $video_cdc_file] ||
    ![get_property USED_IN_IMPLEMENTATION $video_cdc_file] ||
    [get_property PROCESSING_ORDER $video_cdc_file] ne "LATE"} {
    error "Implementation video CDC constraint properties did not apply"
}
