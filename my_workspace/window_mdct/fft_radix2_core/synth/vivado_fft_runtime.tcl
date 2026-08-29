# Full Vivado synthesis, place and route for the pipelined FFT64/FFT512 core.
#
# Run from my_workspace/window_mdct/fft_radix2_core:
#   vivado -mode batch -source synth/vivado_fft_runtime.tcl

set root [file normalize [pwd]]
# PYNQ-Z2 uses the Zynq-7020 device below.  PART_NAME may be supplied by the
# caller when the same RTL is characterized on another FPGA.
if {[info exists ::env(PART_NAME)] && $::env(PART_NAME) ne ""} {
  set part_name $::env(PART_NAME)
} else {
  set part_name "xc7z020clg400-1"
}
puts "Target FPGA part: $part_name"
set project_dir [file join $root .vivado_fft_runtime]

create_project fft_radix2_runtime_impl $project_dir -part $part_name -force
set_property target_language VHDL [current_project]
set_property simulator_language Mixed [current_project]

# Compile order is explicit because all submodules depend on fft_radix2_pkg.
read_vhdl -vhdl2008 [file join $root rtl fft_radix2_pkg.vhd]
read_vhdl -vhdl2008 [file join $root rtl fft_radix2_twiddle_rom.vhd]
read_vhdl -vhdl2008 [file join $root rtl fft_radix2_addr_gen.vhd]
read_vhdl -vhdl2008 [file join $root rtl fft_radix2_memory.vhd]
read_vhdl -vhdl2008 [file join $root rtl fft_radix2_butterfly.vhd]
read_vhdl -vhdl2008 [file join $root rtl fft_radix2_control.vhd]
read_vhdl -vhdl2008 [file join $root rtl fft_radix2_core.vhd]
read_xdc [file join $root synth fft_radix2_core.xdc]

synth_design -top fft_radix2_core -part $part_name \
  -generic DATA_W=32 \
  -generic ENABLE_TRACE=false

report_utilization -hierarchical \
  -file [file join $root synth utilization_post_synth.rpt]
report_dsp_utilization \
  -file [file join $root synth dsp_post_synth.rpt]

# Timing after synth alone is not a valid Fmax result.  Complete implementation
# before emitting the timing and routed utilization reports.
opt_design
place_design
phys_opt_design
route_design

report_timing_summary -delay_type max -max_paths 20 -report_unconstrained \
  -file [file join $root synth timing_post_route.rpt]
report_utilization -hierarchical \
  -file [file join $root synth utilization_post_route.rpt]
report_dsp_utilization \
  -file [file join $root synth dsp_post_route.rpt]
report_methodology \
  -file [file join $root synth methodology_post_route.rpt]
write_checkpoint -force [file join $root synth fft_radix2_core_routed.dcp]

puts "Generated implementation reports:"
puts "  synth/utilization_post_synth.rpt"
puts "  synth/dsp_post_synth.rpt"
puts "  synth/timing_post_route.rpt"
puts "  synth/utilization_post_route.rpt"
puts "  synth/dsp_post_route.rpt"
puts "  synth/methodology_post_route.rpt"
