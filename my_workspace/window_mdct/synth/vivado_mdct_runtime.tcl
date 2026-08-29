# Full Vivado synthesis, placement and routing for the AAC-LC MDCT core.
#
# Run from my_workspace/window_mdct on Linux:
#   vivado -mode batch -source synth/vivado_mdct_runtime.tcl

set mdct_root [file normalize [pwd]]
set fft_root [file join $mdct_root fft_radix2_core]
# PYNQ-Z2 uses the Zynq-7020 device below.  PART_NAME may be supplied by the
# caller when the same RTL is characterized on another FPGA.
if {[info exists ::env(PART_NAME)] && $::env(PART_NAME) ne ""} {
  set part_name $::env(PART_NAME)
} else {
  set part_name "xc7z020clg400-1"
}
puts "Target FPGA part: $part_name"
set project_dir [file join $mdct_root .vivado_mdct_runtime]

create_project mdct_runtime_impl $project_dir -part $part_name -force
set_property target_language VHDL [current_project]
set_property simulator_language Mixed [current_project]

# Packages first, then the standalone FFT IP, then the MDCT wrapper.
read_vhdl -vhdl2008 [file join $fft_root rtl fft_radix2_pkg.vhd]
read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_pkg.vhd]

read_vhdl -vhdl2008 [file join $fft_root rtl fft_radix2_twiddle_rom.vhd]
read_vhdl -vhdl2008 [file join $fft_root rtl fft_radix2_addr_gen.vhd]
read_vhdl -vhdl2008 [file join $fft_root rtl fft_radix2_memory.vhd]
read_vhdl -vhdl2008 [file join $fft_root rtl fft_radix2_butterfly.vhd]
read_vhdl -vhdl2008 [file join $fft_root rtl fft_radix2_control.vhd]
read_vhdl -vhdl2008 [file join $fft_root rtl fft_radix2_core.vhd]

read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_window_rom.vhd]
read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_rotation_rom.vhd]
read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_pcm_memory.vhd]
read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_work_memory.vhd]
read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_fft_cache_memory.vhd]
read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_spectrum_memory.vhd]
read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_window_fold.vhd]
read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_dct4_pre.vhd]
read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_dct4_post.vhd]
read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_control.vhd]
read_vhdl -vhdl2008 [file join $mdct_root rtl mdct_core.vhd]
read_xdc [file join $mdct_root synth mdct_core.xdc]

synth_design -top mdct_core -part $part_name -generic ENABLE_TRACE=false

report_utilization -hierarchical \
  -file [file join $mdct_root synth utilization_post_synth.rpt]
report_dsp_utilization \
  -file [file join $mdct_root synth dsp_post_synth.rpt]

# A routed design, not synthesis alone, is required for a meaningful Fmax.
opt_design
place_design
phys_opt_design
route_design

report_timing_summary -delay_type max -max_paths 20 -report_unconstrained \
  -file [file join $mdct_root synth timing_post_route.rpt]
report_utilization -hierarchical \
  -file [file join $mdct_root synth utilization_post_route.rpt]
report_dsp_utilization \
  -file [file join $mdct_root synth dsp_post_route.rpt]
report_methodology \
  -file [file join $mdct_root synth methodology_post_route.rpt]
write_checkpoint -force [file join $mdct_root synth mdct_core_routed.dcp]

puts "Generated implementation reports under synth/:"
puts "  utilization_post_synth.rpt / dsp_post_synth.rpt"
puts "  timing_post_route.rpt / utilization_post_route.rpt"
puts "  dsp_post_route.rpt / methodology_post_route.rpt"
