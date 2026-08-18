source [file join [file dirname [info script]] timing_run_helpers.tcl]

if {[llength $argv] != 2} {
    error "Usage: vivado -mode batch -source scripts/mark_timing_hardware_validated.tcl -tclargs <build-id> <comma-separated-tests>"
}

lassign $argv build_id tests
if {![timing_run::validate_build_id $build_id]} {
    error "Invalid timing build ID: $build_id"
}

set run_dir [file join .timing_runs $build_id]
set validation_path [file join $run_dir hardware_validation.txt]
if {[file exists $validation_path]} {
    error "Hardware validation already exists for $build_id; refusing to overwrite it."
}
set manifest [timing_run::read_manifest [file join $run_dir manifest.txt]]
timing_run::validate_signoff_manifest $manifest
timing_run::require_manifest_value $manifest build_id $build_id "Validated build ID"
set current_git [timing_run::git_state]
timing_run::require_manifest_value $current_git git_sha \
    [dict get $manifest git_sha] "Current Git commit"
timing_run::require_manifest_value $current_git git_dirty 0 "Current Git dirty flag"

set candidate_path [file join $run_dir candidate.dcp]
set candidate_hash [timing_run::sha256_file $candidate_path]
if {$candidate_hash ne [dict get $manifest candidate_dcp_sha256]} {
    error "The candidate checkpoint does not match its manifest."
}
timing_run::require_file_hash \
    [file join $run_dir appletini_yarz_top.bit] \
    [dict get $manifest bitstream_sha256] "Build bitstream"
timing_run::require_file_hash \
    [file join $run_dir appletini_yarz_top.xsa] \
    [dict get $manifest xsa_sha256] "Build XSA"
timing_run::require_file_hash project/appletini_yarz_top.xsa \
    [dict get $manifest xsa_sha256] "Current Vitis input XSA"

set firmware_manifest_path [file join $run_dir firmware_manifest.txt]
set firmware_manifest [timing_run::read_manifest $firmware_manifest_path]
timing_run::require_manifest_value $firmware_manifest status packaged \
    "Firmware package status"
foreach key {build_id git_sha candidate_dcp_sha256 bitstream_sha256 xsa_sha256} {
    timing_run::require_manifest_value $firmware_manifest $key \
        [dict get $manifest $key] "Firmware package $key"
}
timing_run::require_manifest_value $firmware_manifest firmware_file \
    FIRMWARE.BIN "Firmware package file"
set firmware_path [file join $run_dir FIRMWARE.BIN]
timing_run::require_file_hash $firmware_path \
    [dict get $firmware_manifest firmware_sha256] "Packaged firmware"
foreach key {fsbl_sha256 frontend_elf_sha256 core1_elf_sha256} {
    if {![dict exists $firmware_manifest $key] ||
        [dict get $firmware_manifest $key] in {"" unavailable}} {
        error "Firmware package has no valid $key."
    }
}

set validator "unknown"
foreach name {USERNAME USER} {
    if {[info exists ::env($name)] && $::env($name) ne ""} {
        set validator $::env($name)
        break
    }
}
set tests [string trim $tests]
timing_run::validate_hardware_tests $tests

set validation [dict create \
    validation_status PASS \
    validated_utc [timing_run::utc_now] \
    validator $validator \
    build_id $build_id \
    git_sha [dict get $manifest git_sha] \
    candidate_dcp_sha256 $candidate_hash \
    bitstream_sha256 [dict get $manifest bitstream_sha256] \
    xsa_sha256 [dict get $manifest xsa_sha256] \
    firmware_path [file normalize $firmware_path] \
    firmware_file FIRMWARE.BIN \
    firmware_sha256 [dict get $firmware_manifest firmware_sha256] \
    firmware_manifest_sha256 [timing_run::sha256_file $firmware_manifest_path] \
    fsbl_sha256 [dict get $firmware_manifest fsbl_sha256] \
    frontend_elf_sha256 [dict get $firmware_manifest frontend_elf_sha256] \
    core1_elf_sha256 [dict get $firmware_manifest core1_elf_sha256] \
    hardware_tests $tests]

timing_run::write_manifest $validation_path $validation
puts "Recorded hardware validation for timing build $build_id."
puts "Firmware SHA-256: [dict get $validation firmware_sha256]"
