# Copyright 2025 OpenAirLink Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# generate_fft_ips.tcl
# ====================
#
# Generates two Xilinx Fast Fourier Transform IP cores (xfft v9.1) needed
# by the UPOLS block: one forward FFT, one inverse FFT, both K-point with
# identical fixed-point configuration.
#
# This is a one-shot helper.  Run it once after first cloning the repo;
# Vivado will create the .xci files in this directory and the FPGA build
# will pick them up via Makefile.srcs.
#
# What this produces:
#   ./xfft_K_fwd/xfft_K_fwd.xci   ← forward FFT IP, K-point
#   ./xfft_K_inv/xfft_K_inv.xci   ← inverse FFT IP, K-point
#
# What an .xci file is:
#   It is Xilinx's serialized IP-configuration document — XML describing
#   exactly which Xilinx IP this is and every configuration knob you set.
#   Vivado regenerates the actual HDL (.v) for the IP from the .xci at
#   synthesis time.  You only need the .xci file; no other generated
#   artifacts need to be checked into the repo.
#
# How to run:
#   1. Make sure `vivado` is on your PATH.  If not:
#        source /tools/Xilinx/Vivado/2021.1/settings64.sh
#      (adjust the path to wherever your Vivado 2021.1 is installed)
#
#   2. cd into this directory:
#        cd <repo>/rfnoc-openairlink/fpga/rfnoc_block_upols/ip
#
#   3. Run Vivado in batch (non-GUI) mode with this script:
#        vivado -mode batch -source generate_fft_ips.tcl
#
#   4. Wait ~30–60 seconds.  When you see
#        "FFT IP generation complete."
#      both .xci files are ready.
#
#   5. (Optional) Delete the temp project:
#        rm -rf _vivado_tmp vivado*.jou vivado*.log .Xil
#
# If Vivado complains "ERROR: [Common 17-356] failed to find a part …",
# the TARGET_PART below does not match your installation's loaded board
# files.  Verify the X410 part name and update the TARGET_PART line.
# (As of Vivado 2021.1 with Ettus' X410 BSP installed, the part is
# xczu28dr-ffvg1517-1L-e.)


# ============================================================
# Tunables — change these only if K or the device changes
# ============================================================
set FFT_LENGTH  256
set TARGET_PART "xczu28dr-ffvg1517-1L-e"

# Resolve the directory this script lives in (= the OOT block's IP root)
set ip_root [file normalize [file dirname [info script]]]

# Temporary Vivado project location (deleted at end if you want)
set tmp_prj_dir [file join $ip_root "_vivado_tmp"]
file mkdir $tmp_prj_dir

puts "============================================================"
puts "UPOLS FFT-IP generator"
puts "  IP root           : $ip_root"
puts "  Temp project dir  : $tmp_prj_dir"
puts "  Target part       : $TARGET_PART"
puts "  FFT length (K)    : $FFT_LENGTH"
puts "============================================================"

create_project -force -part $TARGET_PART xfft_gen $tmp_prj_dir


# ============================================================
# Helper: configure one xfft IP
# ============================================================
proc generate_xfft_variant {ip_name fft_length direction ip_root} {
  puts ""
  puts "------------------------------------------------------------"
  puts "Generating $ip_name  (direction = $direction)"
  puts "------------------------------------------------------------"

  # Create the IP directly inside the OOT block's IP directory.  The
  # `-dir` flag tells Vivado to put generated files at $ip_root/$ip_name/
  # instead of inside the temp project — that way the .xci ends up where
  # the build system expects it.
  create_ip -name        xfft        \
            -vendor      xilinx.com  \
            -library     ip          \
            -version     9.1         \
            -module_name $ip_name    \
            -dir         $ip_root

  # Configuration (matches the spec in axi_upols_fft.v / axi_upols_ifft.v)
  set_property -dict [list                                               \
    CONFIG.transform_length                       $fft_length            \
    CONFIG.implementation_options                 pipelined_streaming_io \
    CONFIG.data_format                            fixed_point            \
    CONFIG.input_width                            16                     \
    CONFIG.phase_factor_width                     16                     \
    CONFIG.scaling_options                        scaled                 \
    CONFIG.rounding_modes                         convergent_rounding    \
    CONFIG.transform_direction                    $direction             \
    CONFIG.run_time_configurable_transform_length false                  \
    CONFIG.output_ordering                        natural_order          \
    CONFIG.cyclic_prefix_insertion                false                  \
    CONFIG.has_aclken                             false                  \
    CONFIG.aresetn                                true                   \
    CONFIG.has_xk_index                           false                  \
    CONFIG.has_nfft                               false                  \
  ] [get_ips $ip_name]

  # Generate the synthesizable HDL and simulation models.
  generate_target {synthesis simulation} [get_ips $ip_name]

  puts "  -> [file join $ip_root $ip_name $ip_name.xci]"
}


# ============================================================
# Generate both variants
# ============================================================
generate_xfft_variant "xfft_K_fwd" $FFT_LENGTH "forward" $ip_root
generate_xfft_variant "xfft_K_inv" $FFT_LENGTH "inverse" $ip_root

close_project

puts ""
puts "============================================================"
puts "FFT IP generation complete."
puts ""
puts "  $ip_root/xfft_K_fwd/xfft_K_fwd.xci"
puts "  $ip_root/xfft_K_inv/xfft_K_inv.xci"
puts ""
puts "You can now delete the temp project to keep the tree clean:"
puts "  rm -rf $tmp_prj_dir vivado*.jou vivado*.log .Xil"
puts "============================================================"
