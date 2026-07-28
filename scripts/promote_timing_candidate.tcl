set candidate  ".vivado_cache/appletini_yarz_top_candidate.dcp"
set known_good ".vivado_cache/appletini_yarz_top_known_good.dcp"

if {![file isfile $candidate]} {
    error "No timing-clean candidate checkpoint exists at $candidate"
}

file mkdir [file dirname $known_good]
file copy -force $candidate $known_good
puts "Promoted hardware-validated placement: [file normalize $known_good]"
