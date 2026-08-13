source [file join [file dirname [info script]] timing_run_helpers.tcl]

set known_good ".vivado_cache/appletini_yarz_top_known_good.dcp"

if {[llength $argv] != 2} {
    error "Usage: vivado -mode batch -source scripts/promote_timing_candidate.tcl -tclargs <tested-build-id> <confirm-build-id>"
}

lassign $argv tested_build_id confirm_build_id
foreach build_id [list $tested_build_id $confirm_build_id] {
    if {![timing_run::validate_build_id $build_id]} {
        error "Invalid timing build ID: $build_id"
    }
}
if {$tested_build_id eq $confirm_build_id} {
    error "Promotion needs two distinct full-build IDs."
}

set timing_root ".timing_runs"
set tested_dir [file join $timing_root $tested_build_id]
set confirm_dir [file join $timing_root $confirm_build_id]
set tested [timing_run::read_manifest [file join $tested_dir manifest.txt]]
set confirm [timing_run::read_manifest [file join $confirm_dir manifest.txt]]

timing_run::validate_signoff_manifest $tested
timing_run::validate_signoff_manifest $confirm
timing_run::require_manifest_value $tested build_id $tested_build_id "Tested build ID"
timing_run::require_manifest_value $confirm build_id $confirm_build_id "Confirm build ID"

set tested_sha [dict get $tested git_sha]
set confirm_sha [dict get $confirm git_sha]
if {$tested_sha ne $confirm_sha} {
    error "The two builds must use the same Git commit ($tested_sha != $confirm_sha)."
}
timing_run::require_matching_manifest_values $tested $confirm {
    vivado_version device_part speed_grade seed_control
    synth_strategy synth_retiming control_set_opt_threshold
    impl_strategy place_directive phys_opt_directive route_directive
    post_route_phys_opt_directive jobs rescue_used
}
set current_git [timing_run::git_state]
timing_run::require_manifest_value $current_git git_sha $tested_sha \
    "Current Git commit"
timing_run::require_manifest_value $current_git git_dirty 0 \
    "Current Git dirty flag"
if {[string compare [dict get $tested utc_start] [dict get $confirm utc_start]] >= 0} {
    error "The confirm build must start after the tested build."
}

# A build from another commit, or a failed or partial full build, breaks the
# streak because the chosen pair must be the latest two full attempts.
timing_run::require_latest_full_pair \
    $timing_root $tested_build_id $confirm_build_id

set tested_candidate [file join $tested_dir candidate.dcp]
set tested_candidate_hash [timing_run::sha256_file $tested_candidate]
if {$tested_candidate_hash ne [dict get $tested candidate_dcp_sha256]} {
    error "The tested candidate checkpoint does not match its manifest."
}
set confirm_candidate [file join $confirm_dir candidate.dcp]
set confirm_candidate_hash [timing_run::sha256_file $confirm_candidate]
if {$confirm_candidate_hash ne [dict get $confirm candidate_dcp_sha256]} {
    error "The confirm candidate checkpoint does not match its manifest."
}
foreach pair [list \
    [list appletini_yarz_top.bit bitstream_sha256] \
    [list appletini_yarz_top.xsa xsa_sha256]] {
    lassign $pair file_name hash_key
    timing_run::require_file_hash [file join $tested_dir $file_name] \
        [dict get $tested $hash_key] "Tested build $file_name"
    timing_run::require_file_hash [file join $confirm_dir $file_name] \
        [dict get $confirm $hash_key] "Confirm build $file_name"
}

set validation_path [file join $tested_dir hardware_validation.txt]
set validation [timing_run::read_manifest $validation_path]
timing_run::require_manifest_value $validation validation_status PASS "Hardware validation"
timing_run::require_manifest_value $validation build_id $tested_build_id "Validated build ID"
timing_run::require_manifest_value $validation git_sha $tested_sha "Validated Git commit"
timing_run::require_manifest_value $validation candidate_dcp_sha256 \
    $tested_candidate_hash "Validated checkpoint hash"
timing_run::require_manifest_value $validation bitstream_sha256 \
    [dict get $tested bitstream_sha256] "Validated bitstream hash"
timing_run::require_manifest_value $validation xsa_sha256 \
    [dict get $tested xsa_sha256] "Validated XSA hash"
if {![dict exists $validation firmware_sha256] ||
    [dict get $validation firmware_sha256] in {"" unavailable}} {
    error "Hardware validation has no tested firmware SHA-256."
}
if {![dict exists $validation hardware_tests]} {
    error "Hardware validation has no test list."
}
timing_run::validate_hardware_tests [dict get $validation hardware_tests]
timing_run::require_manifest_value $validation firmware_file \
    FIRMWARE.BIN "Archived firmware name"
set firmware_manifest_path [file join $tested_dir firmware_manifest.txt]
timing_run::require_file_hash $firmware_manifest_path \
    [dict get $validation firmware_manifest_sha256] "Firmware package manifest"
set firmware_manifest [timing_run::read_manifest $firmware_manifest_path]
timing_run::require_manifest_value $firmware_manifest firmware_sha256 \
    [dict get $validation firmware_sha256] "Packaged firmware hash"
set tested_firmware [file join $tested_dir FIRMWARE.BIN]
if {[timing_run::sha256_file $tested_firmware] ne
    [dict get $validation firmware_sha256]} {
    error "Archived tested firmware does not match its hardware record."
}

file mkdir [file dirname $known_good]
file copy -force $tested_candidate $known_good
file copy -force [file join $tested_dir manifest.txt] "${known_good}.manifest.txt"
file copy -force $validation_path "${known_good}.hardware_validation.txt"
puts "Promoted tested timing build $tested_build_id after confirm build $confirm_build_id."
puts "Known-good checkpoint: [file normalize $known_good]"
