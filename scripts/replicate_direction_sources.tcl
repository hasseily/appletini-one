# Give the Apple direction-pin logic its own copy of each source register.
#
# a2fpga_dir_a and a2fpga_dir_d have a 10 ns register-to-pad limit. About 25
# registers all over the fabric feed them through two to four LUT levels:
# machine identity, policy, ONE//e isolation and IIgs ownership. Most of these
# registers also have many other loads, so the placer cannot keep them near
# the two pads, and one of them misses the limit in most runs.
#
# This hook runs before place_design. For each flip-flop that starts a path to
# a direction pad and also has loads outside that logic, it adds a twin
# flip-flop of the same type with the same D, CE, reset and clock nets and the
# same INIT. The twin has the same value as the original in every clock cycle.
# Only the load pins inside the direction-pad logic move to the twin. The
# placer can then put the twin beside the pads.
#
# No logic function, state or bus phase changes. This is the replication that
# phys_opt_design does after placement, done before placement.
namespace eval appletini_dir_twins {
    variable ports {a2fpga_dir_a a2fpga_dir_d}
    variable ff_types {FDRE FDSE FDCE FDPE}
    variable suffix _dirtwin

    proc parent_of {name} {
        set cut [string last / $name]
        if {$cut < 0} {return ""}
        return [string range $name 0 [expr {$cut - 1}]]
    }

    # 1 if the cell or one of its parents refuses netlist edits.
    proc is_protected {cell} {
        set name [get_property NAME $cell]
        while {$name ne ""} {
            set scope [get_cells -quiet $name]
            if {[llength $scope] == 1 &&
                [get_property -quiet DONT_TOUCH $scope] eq "1"} {
                return 1
            }
            set name [parent_of $name]
        }
        return 0
    }

    proc run {} {
        variable ports
        variable ff_types
        variable suffix
        set pads [get_ports -quiet $ports]
        if {[llength $pads] != [llength $ports]} {
            error "Expected both Apple direction ports."
        }
        if {[llength [get_cells -quiet -hierarchical -filter \
                "NAME =~ *$suffix"]] != 0} {
            error "Direction-source twins already exist."
        }

        # Every pin on a path to the two pads.
        array set in_cone {}
        foreach pin [all_fanin -flat $pads] {
            set in_cone($pin) 1
        }

        set made {}
        set skipped {}
        foreach start [all_fanin -flat -startpoints_only $pads] {
            set cell [get_cells -quiet -of_objects $start]
            if {[llength $cell] != 1} {continue}
            set ref [get_property REF_NAME $cell]
            if {[lsearch -exact $ff_types $ref] < 0} {
                lappend skipped "$cell ($ref)"
                continue
            }
            if {[is_protected $cell]} {
                lappend skipped "$cell (DONT_TOUCH)"
                continue
            }
            set q_net [get_nets -quiet -of_objects [get_pins $cell/Q]]
            if {[llength $q_net] != 1} {continue}
            set cone_loads {}
            set other_loads 0
            foreach load [get_pins -quiet -leaf -of_objects $q_net \
                    -filter {DIRECTION == IN}] {
                if {[info exists in_cone($load)]} {
                    lappend cone_loads $load
                } else {
                    incr other_loads
                }
            }
            if {[llength $cone_loads] == 0 || $other_loads == 0} {
                # Already private to the direction logic, or not a real source.
                continue
            }

            set twin_name "[get_property NAME $cell]$suffix"
            create_cell -reference $ref $twin_name
            set twin [get_cells $twin_name]
            foreach property {INIT IS_C_INVERTED IS_D_INVERTED IS_R_INVERTED
                              IS_S_INVERTED IS_CLR_INVERTED IS_PRE_INVERTED} {
                set value [get_property -quiet $property $cell]
                if {$value ne ""} {
                    set_property $property $value $twin
                }
            }
            foreach pin [get_pins -of_objects $cell -filter {DIRECTION == IN}] {
                set net [get_nets -quiet -of_objects $pin]
                if {[llength $net] != 1} {
                    error "Unconnected input on direction source: $pin"
                }
                connect_net -hier -net $net -objects \
                    [get_pins $twin_name/[get_property REF_PIN_NAME $pin]]
            }
            set twin_net "${twin_name}_q"
            create_net $twin_net
            connect_net -net [get_nets $twin_net] -objects [get_pins $twin_name/Q]
            foreach load $cone_loads {
                disconnect_net -net [get_nets -of_objects $load] -objects $load
            }
            connect_net -hier -net [get_nets $twin_net] -objects $cone_loads
            lappend made [list $cell $twin $cone_loads $other_loads]
        }

        # Read the result back from the netlist.
        foreach item $made {
            lassign $item cell twin cone_loads other_loads
            foreach property {REF_NAME INIT IS_C_INVERTED IS_D_INVERTED
                              IS_R_INVERTED IS_S_INVERTED IS_CLR_INVERTED
                              IS_PRE_INVERTED} {
                if {[get_property -quiet $property $cell] ne
                    [get_property -quiet $property $twin]} {
                    error "Twin differs in $property: $twin"
                }
            }
            foreach pin [get_pins -of_objects $cell -filter {DIRECTION == IN}] {
                set twin_pin [get_pins \
                    [get_property NAME $twin]/[get_property REF_PIN_NAME $pin]]
                # Compare drivers: one net has a different name at each level.
                set driver [get_pins -quiet -leaf -of_objects \
                    [get_nets -of_objects $pin] -filter {DIRECTION == OUT}]
                set twin_driver [get_pins -quiet -leaf -of_objects \
                    [get_nets -of_objects $twin_pin] -filter {DIRECTION == OUT}]
                if {$driver ne $twin_driver ||
                    [get_property -quiet TYPE [get_nets -of_objects $pin]] ne
                    [get_property -quiet TYPE [get_nets -of_objects $twin_pin]]} {
                    error "Twin input differs from its original: $twin_pin"
                }
            }
            set twin_loads [get_pins -quiet -leaf -of_objects \
                [get_nets -of_objects [get_pins [get_property NAME $twin]/Q]] \
                -filter {DIRECTION == IN}]
            if {[lsort $twin_loads] ne [lsort $cone_loads]} {
                error "Twin loads differ from the moved loads: $twin"
            }
            foreach load $twin_loads {
                if {![info exists in_cone($load)]} {
                    error "Twin drives a pin outside the direction logic: $load"
                }
            }
            set left [get_pins -quiet -leaf -of_objects \
                [get_nets -of_objects [get_pins [get_property NAME $cell]/Q]] \
                -filter {DIRECTION == IN}]
            if {[llength $left] != $other_loads} {
                error "Original lost a load outside the direction logic: $cell"
            }
            puts "Direction-source twin: $twin ([llength $cone_loads] loads moved, $other_loads kept)"
        }
        foreach item $skipped {
            puts "Direction source without twin: $item"
        }
        if {[llength $made] == 0} {
            error "No direction-source twin was made."
        }
        puts "Direction-source twins made: [llength $made]"

        # Keep the twins in the area of the direction gates. A twin has no
        # load outside the direction logic, but its D net pulls it to its
        # original. In runs that failed, the twins were at rows 39 to 43,
        # 10 to 20 rows from the gates; in runs that passed, at row 34 or
        # below. The area is not exclusive, so other logic can use it.
        set twins {}
        foreach item $made {
            lappend twins [lindex $item 1]
        }
        set area [create_pblock pblock_dir_twins]
        resize_pblock $area -add {SLICE_X98Y20:SLICE_X113Y36}
        add_cells_to_pblock $area $twins
        puts "Direction-source twins in pblock_dir_twins: [llength $twins]"
    }
}
appletini_dir_twins::run
