namespace eval timing_run {
    variable root_dir ".timing_runs"
    variable required_hardware_tests {
        boot-menu disk-ii smartport vtw mb-audit
        linear-overlay sdd uthernet ssc reset
    }
    variable builds_header {
        build_id utc_start utc_end status git_sha git_dirty vivado_version
        build_mode incremental_reference incremental_reference_sha256
        device_part speed_grade seed_control
        synth_strategy synth_retiming control_set_opt_threshold
        impl_strategy place_directive phys_opt_directive route_directive
        post_route_phys_opt_directive jobs rescue_used
        wns_ns tns_ns whs_ns ths_ns wpws_ns tpws_ns
        setup_failing_endpoints hold_failing_endpoints
        pulse_width_failing_endpoints
        unconstrained_internal_endpoints route_status route_errors
        bus_skew_status bus_skew_wns_ns
        missing_constraint_objects
        slices luts lut_logic lut_memory shift_register_luts registers
        f7_muxes f8_muxes carry4s
        bram_tiles ramb36 ramb18 dsps control_sets
        candidate_dcp_sha256 bitstream_sha256 xsa_sha256
    }
    variable paths_header {
        build_id rank startpoint endpoint start_parent endpoint_parent
        start_module endpoint_module
        start_cell_type endpoint_cell_type
        path_group start_clock endpoint_clock slack_ns requirement_ns
        datapath_delay_ns logic_delay_ns route_delay_ns route_percent
        logic_levels max_fanout skew_ns uncertainty_ns
    }
}

proc timing_run::env_enabled {name} {
    if {![info exists ::env($name)]} {
        return 0
    }
    set value [string trim $::env($name)]
    return [expr {$value ne "" && $value ne "0"}]
}

proc timing_run::safe_property {object property {default ""}} {
    if {[llength $object] == 0} {
        return $default
    }
    if {[catch {get_property $property $object} value]} {
        return $default
    }
    return $value
}

proc timing_run::git_value {args default} {
    if {[catch {exec git {*}$args} value]} {
        return $default
    }
    return [string trim $value]
}

proc timing_run::git_state {} {
    set sha [git_value {rev-parse HEAD} "unknown"]
    set short_sha [string range $sha 0 7]
    # Include untracked source files. Ignored build products do not appear,
    # while a new RTL or constraint file must prevent signoff as a clean tree.
    set dirty_text [git_value {status --porcelain --untracked-files=normal} "unknown"]
    if {$dirty_text eq "unknown"} {
        set dirty "unknown"
    } else {
        set dirty [expr {$dirty_text ne "" ? 1 : 0}]
    }
    return [dict create git_sha $sha short_sha $short_sha git_dirty $dirty]
}

proc timing_run::utc_now {{id_format 0}} {
    if {$id_format} {
        return [clock format [clock seconds] -gmt 1 -format "%Y%m%dT%H%M%SZ"]
    }
    return [clock format [clock seconds] -gmt 1 -format "%Y-%m-%dT%H:%M:%SZ"]
}

proc timing_run::new_build {mode} {
    variable root_dir
    set git [git_state]
    set base "[utc_now 1]-[dict get $git short_sha]-$mode"
    set build_id $base
    set suffix 1
    while {[file exists [file join $root_dir $build_id]]} {
        set build_id [format "%s-%02d" $base $suffix]
        incr suffix
    }
    set run_dir [file join $root_dir $build_id]
    file mkdir $run_dir
    return [dict merge $git [dict create \
        build_id $build_id \
        run_dir $run_dir \
        utc_start [utc_now] \
        utc_end "" \
        status started \
        build_mode $mode]]
}

proc timing_run::sha256_file {path} {
    if {![file isfile $path]} {
        return ""
    }
    set normalized [file normalize $path]
    if {![catch {exec certutil -hashfile $normalized SHA256} output]} {
        foreach line [split $output "\n"] {
            set compact [string map {" " "" "\r" "" "\t" ""} $line]
            if {[regexp -nocase {^[0-9a-f]{64}$} $compact]} {
                return [string tolower $compact]
            }
        }
    }
    if {![catch {exec sha256sum $normalized} output] &&
        [regexp -nocase {^([0-9a-f]{64})} $output -> digest]} {
        return [string tolower $digest]
    }
    return "unavailable"
}

proc timing_run::validate_manifest_entry {key value} {
    if {![regexp {^[A-Za-z][A-Za-z0-9_.-]*$} $key]} {
        error "Invalid manifest key: $key"
    }
    if {[string first "\r" $value] >= 0 ||
        [string first "\n" $value] >= 0 ||
        [string first "\0" $value] >= 0} {
        error "Manifest value for $key contains a line break or NUL"
    }
}

proc timing_run::write_manifest {path values} {
    foreach key [dict keys $values] {
        validate_manifest_entry $key [dict get $values $key]
    }
    set handle [open $path w]
    fconfigure $handle -encoding utf-8 -translation lf
    try {
        foreach key [lsort [dict keys $values]] {
            set value [dict get $values $key]
            puts $handle "$key=$value"
        }
    } finally {
        close $handle
    }
}

proc timing_run::read_text {path} {
    set handle [open $path r]
    try {
        return [read $handle]
    } finally {
        close $handle
    }
}

proc timing_run::read_manifest {path} {
    if {![file isfile $path]} {
        error "Manifest does not exist: $path"
    }
    set values [dict create]
    set handle [open $path r]
    fconfigure $handle -encoding utf-8 -translation binary
    try {
        set text [read $handle]
        if {[string first "\r" $text] >= 0 ||
            [string first "\0" $text] >= 0} {
            error "Manifest must contain UTF-8 LF text only: $path"
        }
        set line_number 0
        foreach line [split $text "\n"] {
            incr line_number
            if {$line eq "" || [string index $line 0] eq "#"} {
                continue
            }
            set separator [string first "=" $line]
            if {$separator < 1} {
                error "Malformed manifest line in $path: $line"
            }
            set key [string range $line 0 [expr {$separator - 1}]]
            set value [string range $line [expr {$separator + 1}] end]
            validate_manifest_entry $key $value
            if {[dict exists $values $key]} {
                error "Duplicate manifest key $key on line $line_number in $path"
            }
            dict set values $key $value
        }
    } finally {
        close $handle
    }
    return $values
}

proc timing_run::csv_field {value} {
    set value [string map {"\r" " " "\n" " " "\"" "\"\""} $value]
    return "\"$value\""
}

proc timing_run::append_csv_dict {path header values} {
    file mkdir [file dirname $path]
    set needs_header [expr {![file exists $path] || [file size $path] == 0}]
    set handle [open $path a]
    try {
        if {$needs_header} {
            puts $handle [join $header ,]
        }
        set fields {}
        foreach key $header {
            if {[dict exists $values $key]} {
                lappend fields [csv_field [dict get $values $key]]
            } else {
                lappend fields [csv_field ""]
            }
        }
        puts $handle [join $fields ,]
    } finally {
        close $handle
    }
}

proc timing_run::append_build {values} {
    variable root_dir
    variable builds_header
    append_csv_dict [file join $root_dir builds.csv] $builds_header $values
}

proc timing_run::append_path {values} {
    variable root_dir
    variable paths_header
    append_csv_dict [file join $root_dir paths.csv] $paths_header $values
}

proc timing_run::parse_timing_summary {text} {
    set values [dict create \
        wns_ns "" tns_ns "" whs_ns "" ths_ns "" wpws_ns "" tpws_ns "" \
        setup_failing_endpoints "" hold_failing_endpoints "" \
        pulse_width_failing_endpoints "" \
        unconstrained_internal_endpoints "unknown"]
    set number {[-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)}
    foreach line [split $text "\n"] {
        if {[regexp "^\\s*($number)\\s+($number)\\s+(\\d+)\\s+(\\d+)\\s+($number)\\s+($number)\\s+(\\d+)\\s+(\\d+)\\s+($number)\\s+($number)\\s+(\\d+)\\s+(\\d+)\\s*$" \
            $line -> wns tns setup_failing setup_total whs ths \
            hold_failing hold_total wpws tpws pulse_failing pulse_total]} {
            dict set values wns_ns $wns
            dict set values tns_ns $tns
            dict set values whs_ns $whs
            dict set values ths_ns $ths
            dict set values wpws_ns $wpws
            dict set values tpws_ns $tpws
            dict set values setup_failing_endpoints $setup_failing
            dict set values hold_failing_endpoints $hold_failing
            dict set values pulse_width_failing_endpoints $pulse_failing
            break
        }
    }
    if {[regexp {checking unconstrained_internal_endpoints \(([0-9]+)\)} $text -> count]} {
        dict set values unconstrained_internal_endpoints $count
    }
    return $values
}

proc timing_run::parse_used_value {text label} {
    set escaped [regsub -all {([][(){}.^$*+?|\\])} $label {\\\1}]
    set pattern [format {^\|\s*%s\s*\|\s*([0-9.]+)\s*\|} $escaped]
    if {[regexp -line $pattern $text -> used]} {
        return $used
    }
    return ""
}

proc timing_run::parse_utilization {text} {
    return [dict create \
        slices [parse_used_value $text "Slice"] \
        luts [parse_used_value $text "Slice LUTs"] \
        lut_logic [parse_used_value $text "LUT as Logic"] \
        lut_memory [parse_used_value $text "LUT as Memory"] \
        shift_register_luts [parse_used_value $text "LUT as Shift Register"] \
        registers [parse_used_value $text "Slice Registers"] \
        f7_muxes [parse_used_value $text "F7 Muxes"] \
        f8_muxes [parse_used_value $text "F8 Muxes"] \
        carry4s [parse_used_value $text "CARRY4"] \
        bram_tiles [parse_used_value $text "Block RAM Tile"] \
        ramb36 [parse_used_value $text "RAMB36E1 only"] \
        ramb18 [parse_used_value $text "RAMB18E1 only"] \
        dsps [parse_used_value $text "DSPs"] \
        control_sets [parse_used_value $text "Unique Control Sets"]]
}

proc timing_run::parent_name {pin_name} {
    if {$pin_name eq ""} {
        return ""
    }
    set slash [string last "/" $pin_name]
    if {$slash < 0} {
        return ""
    }
    return [string range $pin_name 0 [expr {$slash - 1}]]
}

proc timing_run::parse_route_status {text} {
    set errors "unknown"
    if {[regexp {# of nets with routing errors[^:]*:\s*([0-9]+)} $text -> count]} {
        set errors $count
    }
    if {$errors eq "unknown"} {
        set status UNKNOWN
    } elseif {$errors == 0} {
        set status PASS
    } else {
        set status FAIL
    }
    return [dict create route_errors $errors route_status $status]
}

proc timing_run::parse_bus_skew {text} {
    set slacks {}
    foreach line [split $text "\n"] {
        if {[regexp {\s(?:Slow|Fast)\s+[-+0-9.]+\s+[-+0-9.]+\s+([-+0-9.]+)\s*$} $line -> slack]} {
            lappend slacks $slack
        }
    }
    if {[llength $slacks] == 0} {
        return [dict create bus_skew_status UNKNOWN bus_skew_wns_ns ""]
    }
    set worst [lindex $slacks 0]
    foreach slack [lrange $slacks 1 end] {
        if {$slack < $worst} {
            set worst $slack
        }
    }
    return [dict create \
        bus_skew_status [expr {$worst >= 0.0 ? "PASS" : "FAIL"}] \
        bus_skew_wns_ns $worst]
}

proc timing_run::count_missing_constraint_objects {text} {
    set count 0
    foreach line [split $text "\n"] {
        # Vivado echoes sourced Tcl into its log. Match only tool messages so
        # this procedure does not count its own pattern text.
        if {[regexp -nocase {^\s*(?:CRITICAL WARNING|WARNING|ERROR):.*(?:No valid object\(s\) found|matched no objects|expects at least one object)} $line]} {
            incr count
        }
    }
    return $count
}

proc timing_run::pin_cell_type {pin_name} {
    if {$pin_name eq ""} {
        return ""
    }
    set pins [get_pins -quiet $pin_name]
    set cells [get_cells -quiet -of_objects $pins]
    return [safe_property $cells REF_NAME ""]
}

proc timing_run::enclosing_module {pin_name} {
    set cell_name [parent_name $pin_name]
    while {$cell_name ne ""} {
        set cell_name [parent_name $cell_name]
        if {$cell_name eq ""} {
            break
        }
        set cell [get_cells -quiet $cell_name]
        if {[llength $cell] == 0} {
            continue
        }
        if {![safe_property $cell IS_PRIMITIVE 1]} {
            set ref_name [safe_property $cell REF_NAME ""]
            if {$ref_name ne ""} {
                return "$cell_name ($ref_name)"
            }
            return $cell_name
        }
    }
    return ""
}

proc timing_run::path_values {build_id rank path} {
    set logic_delay [safe_property $path DATAPATH_LOGIC_DELAY ""]
    set route_delay [safe_property $path DATAPATH_NET_DELAY ""]
    set datapath_delay [safe_property $path DATAPATH_DELAY ""]
    set route_percent ""
    if {$datapath_delay ne "" && $datapath_delay != 0.0 && $route_delay ne ""} {
        set route_percent [format "%.1f" [expr {100.0 * $route_delay / $datapath_delay}]]
    }
    set startpoint [safe_property $path STARTPOINT_PIN ""]
    set endpoint [safe_property $path ENDPOINT_PIN ""]
    return [dict create \
        build_id $build_id rank $rank \
        startpoint $startpoint endpoint $endpoint \
        start_parent [parent_name $startpoint] \
        endpoint_parent [parent_name $endpoint] \
        start_module [enclosing_module $startpoint] \
        endpoint_module [enclosing_module $endpoint] \
        start_cell_type [pin_cell_type $startpoint] \
        endpoint_cell_type [pin_cell_type $endpoint] \
        path_group [safe_property $path GROUP ""] \
        start_clock [safe_property $path STARTPOINT_CLOCK ""] \
        endpoint_clock [safe_property $path ENDPOINT_CLOCK ""] \
        slack_ns [safe_property $path SLACK ""] \
        requirement_ns [safe_property $path REQUIREMENT ""] \
        datapath_delay_ns $datapath_delay \
        logic_delay_ns $logic_delay route_delay_ns $route_delay \
        route_percent $route_percent \
        logic_levels [safe_property $path LOGIC_LEVELS ""] \
        max_fanout [safe_property $path MAX_FANOUT ""] \
        skew_ns [safe_property $path SKEW ""] \
        uncertainty_ns [safe_property $path UNCERTAINTY ""]]
}

proc timing_run::require_manifest_value {values key expected description} {
    if {![dict exists $values $key]} {
        error "$description is missing ($key)."
    }
    set actual [dict get $values $key]
    if {$actual ne $expected} {
        error "$description must be '$expected', got '$actual'."
    }
}

proc timing_run::require_number_at_least {values key minimum description} {
    if {![dict exists $values $key] || ![string is double -strict [dict get $values $key]]} {
        error "$description is missing or invalid ($key)."
    }
    set actual [dict get $values $key]
    if {$actual < $minimum} {
        error "$description must be at least $minimum, got $actual."
    }
}

proc timing_run::require_number_zero {values key description} {
    if {![dict exists $values $key] ||
        ![string is double -strict [dict get $values $key]]} {
        error "$description is missing or invalid ($key)."
    }
    set actual [dict get $values $key]
    if {$actual != 0.0} {
        error "$description must be zero, got $actual."
    }
}

proc timing_run::require_file_hash {path expected description} {
    if {$expected in {"" unavailable}} {
        error "$description has no valid expected SHA-256."
    }
    set actual [sha256_file $path]
    if {$actual ne $expected} {
        error "$description hash mismatch for $path."
    }
}

proc timing_run::require_matching_manifest_values {first second keys} {
    foreach key $keys {
        if {![dict exists $first $key] || ![dict exists $second $key]} {
            error "Build manifests do not both contain '$key'."
        }
        set first_value [dict get $first $key]
        set second_value [dict get $second $key]
        if {$first_value ne $second_value} {
            error "Build setting '$key' differs ('$first_value' != '$second_value')."
        }
    }
}

proc timing_run::validate_hardware_tests {tests_text} {
    variable required_hardware_tests
    set tests_text [string trim $tests_text]
    if {$tests_text eq ""} {
        error "Hardware validation has an empty test list."
    }
    set completed_tests [split $tests_text ,]
    foreach required_test $required_hardware_tests {
        if {[lsearch -exact $completed_tests $required_test] < 0} {
            error "Hardware validation is missing required test '$required_test'."
        }
    }
    return $completed_tests
}

proc timing_run::compare_build_order {left right} {
    set time_order [string compare [lindex $left 0] [lindex $right 0]]
    if {$time_order != 0} {
        return $time_order
    }
    return [string compare [lindex $left 1] [lindex $right 1]]
}

proc timing_run::require_latest_full_pair {timing_root tested_id confirm_id} {
    set full_builds {}
    foreach run_dir [glob -nocomplain -types d [file join $timing_root *]] {
        set build_id [file tail $run_dir]
        if {![validate_build_id $build_id] ||
            [lindex [split $build_id -] 2] ne "full"} {
            continue
        }
        set manifest_path [file join $run_dir manifest.txt]
        if {![file isfile $manifest_path]} {
            error "Full build $build_id has no manifest; promotion history is incomplete."
        }
        if {[catch {read_manifest $manifest_path} build]} {
            error "Full build $build_id has an invalid manifest; promotion history is incomplete."
        }
        foreach key {build_id build_mode git_sha utc_start} {
            if {![dict exists $build $key]} {
                error "Full build $build_id manifest has no $key."
            }
        }
        if {[dict get $build build_id] ne $build_id ||
            [dict get $build build_mode] ne "full"} {
            error "Full build $build_id manifest does not match its directory."
        }
        lappend full_builds [list [dict get $build utc_start] $build_id]
    }
    set full_builds [lsort -command timing_run::compare_build_order $full_builds]
    if {[llength $full_builds] < 2 ||
        [lindex $full_builds end-1 1] ne $tested_id ||
        [lindex $full_builds end 1] ne $confirm_id} {
        error "The tested and confirm builds must be the two latest full builds."
    }
}

proc timing_run::validate_signoff_manifest {values} {
    require_manifest_value $values status exported "Build status"
    require_manifest_value $values build_mode full "Build mode"
    require_manifest_value $values git_dirty 0 "Git dirty flag"
    require_manifest_value $values rescue_used 0 "Timing rescue use"
    require_manifest_value $values incremental_reference "" \
        "Incremental reference"
    require_manifest_value $values incremental_reference_sha256 "" \
        "Incremental reference hash"
    require_number_at_least $values wns_ns 0.300 "Setup slack"
    require_number_at_least $values whs_ns 0.000 "Hold slack"
    require_number_at_least $values wpws_ns 0.000 "Pulse-width slack"
    foreach key {
        tns_ns ths_ns tpws_ns setup_failing_endpoints
        hold_failing_endpoints pulse_width_failing_endpoints
        unconstrained_internal_endpoints route_errors
        missing_constraint_objects
    } {
        require_number_zero $values $key "Signoff value $key"
    }
    require_manifest_value $values route_status PASS "Route status"
    require_manifest_value $values bus_skew_status PASS "Bus-skew status"
    foreach key {
        vivado_version device_part speed_grade seed_control
        synth_strategy synth_retiming control_set_opt_threshold
        impl_strategy place_directive phys_opt_directive route_directive
        post_route_phys_opt_directive jobs
    } {
        if {![dict exists $values $key] || [dict get $values $key] eq ""} {
            error "Build flow value is missing ($key)."
        }
    }
    foreach key {candidate_dcp_sha256 bitstream_sha256 xsa_sha256} {
        if {![dict exists $values $key] ||
            [dict get $values $key] in {"" unavailable}} {
            error "Build artifact hash is missing ($key)."
        }
    }
}

proc timing_run::validate_build_id {build_id} {
    set parts [split $build_id -]
    if {[llength $parts] ni {3 4}} {
        return 0
    }
    lassign $parts timestamp sha mode suffix
    if {![regexp {^[0-9]{8}T[0-9]{6}Z$} $timestamp] ||
        ![regexp {^[0-9a-f]{8}$} $sha] ||
        $mode ni {full incremental}} {
        return 0
    }
    if {[llength $parts] == 4 && ![regexp {^[0-9]{2}$} $suffix]} {
        return 0
    }
    return 1
}
