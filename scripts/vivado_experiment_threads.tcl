# Runs in each child process, where Windows otherwise defaults to two threads.
set experiment_threads 8
if {[info exists ::env(APPLETINI_EXPERIMENT_THREADS)]} {
    set experiment_threads $::env(APPLETINI_EXPERIMENT_THREADS)
}
if {![string is integer -strict $experiment_threads] ||
    $experiment_threads < 1 || $experiment_threads > 8} {
    error "APPLETINI_EXPERIMENT_THREADS must be from 1 to 8."
}
set_param general.maxThreads $experiment_threads
puts "Experiment worker threads: [get_param general.maxThreads]"
