# Build for the desktop-opus addon — libopus (xiph/opus @ 55513e81, see
# PINS.md) WITH deep PLC (FARGAN) AND OSCE (LACE/NoLACE).
#
# Derived from qaudion-desktop's native/binding.gyp (private repo, deep-PLC
# only) — same target name, same include layout, same compiler flags — plus
# the 5 extra dnn/*.c files OSCE needs. Built HERE, on a public repo's free
# runners, so the owner's private-repo Actions minutes are not spent
# recompiling libopus (same reasoning as the m150 WebRTC builds in this same
# repo). See ../README.md and PINS.md.
#
# Sources are hand-listed (no vendored tree to generate them from at
# checkout time — fetch-opus-source.sh populates opus/ during the CI job,
# after this file already exists in the repo), cross-checked against:
#   - qaudion-desktop's own binding.gyp (deep-PLC file list, verbatim)
#   - m150/patches/P4a-opus-dnn-osce-build.patch (OSCE file list: xiph/opus's
#     own OSCE_SOURCES minus dnn/bbwenet_data.c, which does not exist at
#     this Opus revision)
#   - upstream xiph/opus's OWN build description at the pinned commit
#     (celt_sources.mk CELT_SOURCES, silk_sources.mk SILK_SOURCES +
#     SILK_SOURCES_FLOAT, opus_sources.mk OPUS_SOURCES + OPUS_SOURCES_FLOAT,
#     lpcnet_sources.mk DEEP_PLC_SOURCES + OSCE_SOURCES — fetched from
#     raw.githubusercontent.com/xiph/opus/55513e81.../*_sources.mk and diffed
#     file-for-file against this list; every path below was also confirmed to
#     exist in the pinned commit's git tree via the GitHub trees API). This
#     is what caught celt/mini_kfft.c: it was in an earlier version of this
#     list but is not a real xiph/opus file at any revision (not in
#     CELT_SOURCES, not SIMD/RTCD either) and does not exist in the pinned
#     commit's tree — MSBuild's C1083 on that path was the tell. Removed.
#
# No SIMD sources and no OPUS_HAVE_RTCD, matching the private repo and iOS/
# Android: the DNN and codec kernels resolve to plain C on every platform.
{
  "targets": [
    {
      "target_name": "qaudion_opus",
      "sources": [
        "opus_addon.cc",
        "opus/celt/bands.c",
        "opus/celt/celt.c",
        "opus/celt/celt_decoder.c",
        "opus/celt/celt_encoder.c",
        "opus/celt/celt_lpc.c",
        "opus/celt/cwrs.c",
        "opus/celt/entcode.c",
        "opus/celt/entdec.c",
        "opus/celt/entenc.c",
        "opus/celt/kiss_fft.c",
        "opus/celt/laplace.c",
        "opus/celt/mathops.c",
        "opus/celt/mdct.c",
        "opus/celt/modes.c",
        "opus/celt/pitch.c",
        "opus/celt/quant_bands.c",
        "opus/celt/rate.c",
        "opus/celt/vq.c",
        "opus/silk/A2NLSF.c",
        "opus/silk/CNG.c",
        "opus/silk/HP_variable_cutoff.c",
        "opus/silk/LPC_analysis_filter.c",
        "opus/silk/LPC_fit.c",
        "opus/silk/LPC_inv_pred_gain.c",
        "opus/silk/LP_variable_cutoff.c",
        "opus/silk/NLSF2A.c",
        "opus/silk/NLSF_VQ.c",
        "opus/silk/NLSF_VQ_weights_laroia.c",
        "opus/silk/NLSF_decode.c",
        "opus/silk/NLSF_del_dec_quant.c",
        "opus/silk/NLSF_encode.c",
        "opus/silk/NLSF_stabilize.c",
        "opus/silk/NLSF_unpack.c",
        "opus/silk/NSQ.c",
        "opus/silk/NSQ_del_dec.c",
        "opus/silk/PLC.c",
        "opus/silk/VAD.c",
        "opus/silk/VQ_WMat_EC.c",
        "opus/silk/ana_filt_bank_1.c",
        "opus/silk/biquad_alt.c",
        "opus/silk/bwexpander.c",
        "opus/silk/bwexpander_32.c",
        "opus/silk/check_control_input.c",
        "opus/silk/code_signs.c",
        "opus/silk/control_SNR.c",
        "opus/silk/control_audio_bandwidth.c",
        "opus/silk/control_codec.c",
        "opus/silk/debug.c",
        "opus/silk/dec_API.c",
        "opus/silk/decode_core.c",
        "opus/silk/decode_frame.c",
        "opus/silk/decode_indices.c",
        "opus/silk/decode_parameters.c",
        "opus/silk/decode_pitch.c",
        "opus/silk/decode_pulses.c",
        "opus/silk/decoder_set_fs.c",
        "opus/silk/enc_API.c",
        "opus/silk/encode_indices.c",
        "opus/silk/encode_pulses.c",
        "opus/silk/gain_quant.c",
        "opus/silk/init_decoder.c",
        "opus/silk/init_encoder.c",
        "opus/silk/inner_prod_aligned.c",
        "opus/silk/interpolate.c",
        "opus/silk/lin2log.c",
        "opus/silk/log2lin.c",
        "opus/silk/pitch_est_tables.c",
        "opus/silk/process_NLSFs.c",
        "opus/silk/quant_LTP_gains.c",
        "opus/silk/resampler.c",
        "opus/silk/resampler_down2.c",
        "opus/silk/resampler_down2_3.c",
        "opus/silk/resampler_private_AR2.c",
        "opus/silk/resampler_private_IIR_FIR.c",
        "opus/silk/resampler_private_down_FIR.c",
        "opus/silk/resampler_private_up2_HQ.c",
        "opus/silk/resampler_rom.c",
        "opus/silk/shell_coder.c",
        "opus/silk/sigm_Q15.c",
        "opus/silk/sort.c",
        "opus/silk/stereo_LR_to_MS.c",
        "opus/silk/stereo_MS_to_LR.c",
        "opus/silk/stereo_decode_pred.c",
        "opus/silk/stereo_encode_pred.c",
        "opus/silk/stereo_find_predictor.c",
        "opus/silk/stereo_quant_pred.c",
        "opus/silk/sum_sqr_shift.c",
        "opus/silk/table_LSF_cos.c",
        "opus/silk/tables_LTP.c",
        "opus/silk/tables_NLSF_CB_NB_MB.c",
        "opus/silk/tables_NLSF_CB_WB.c",
        "opus/silk/tables_gain.c",
        "opus/silk/tables_other.c",
        "opus/silk/tables_pitch_lag.c",
        "opus/silk/tables_pulses_per_block.c",
        "opus/silk/float/LPC_analysis_filter_FLP.c",
        "opus/silk/float/LPC_inv_pred_gain_FLP.c",
        "opus/silk/float/LTP_analysis_filter_FLP.c",
        "opus/silk/float/LTP_scale_ctrl_FLP.c",
        "opus/silk/float/apply_sine_window_FLP.c",
        "opus/silk/float/autocorrelation_FLP.c",
        "opus/silk/float/burg_modified_FLP.c",
        "opus/silk/float/bwexpander_FLP.c",
        "opus/silk/float/corrMatrix_FLP.c",
        "opus/silk/float/encode_frame_FLP.c",
        "opus/silk/float/energy_FLP.c",
        "opus/silk/float/find_LPC_FLP.c",
        "opus/silk/float/find_LTP_FLP.c",
        "opus/silk/float/find_pitch_lags_FLP.c",
        "opus/silk/float/find_pred_coefs_FLP.c",
        "opus/silk/float/inner_product_FLP.c",
        "opus/silk/float/k2a_FLP.c",
        "opus/silk/float/noise_shape_analysis_FLP.c",
        "opus/silk/float/pitch_analysis_core_FLP.c",
        "opus/silk/float/process_gains_FLP.c",
        "opus/silk/float/regularize_correlations_FLP.c",
        "opus/silk/float/residual_energy_FLP.c",
        "opus/silk/float/scale_copy_vector_FLP.c",
        "opus/silk/float/scale_vector_FLP.c",
        "opus/silk/float/schur_FLP.c",
        "opus/silk/float/sort_FLP.c",
        "opus/silk/float/warped_autocorrelation_FLP.c",
        "opus/silk/float/wrappers_FLP.c",
        "opus/src/analysis.c",
        "opus/src/extensions.c",
        "opus/src/mapping_matrix.c",
        "opus/src/mlp.c",
        "opus/src/mlp_data.c",
        "opus/src/opus.c",
        "opus/src/opus_decoder.c",
        "opus/src/opus_encoder.c",
        "opus/src/opus_multistream.c",
        "opus/src/opus_multistream_decoder.c",
        "opus/src/opus_multistream_encoder.c",
        "opus/src/opus_projection_decoder.c",
        "opus/src/opus_projection_encoder.c",
        "opus/src/repacketizer.c",
        "opus/dnn/burg.c",
        "opus/dnn/fargan.c",
        "opus/dnn/fargan_data.c",
        "opus/dnn/freq.c",
        "opus/dnn/lpcnet_enc.c",
        "opus/dnn/lpcnet_plc.c",
        "opus/dnn/lpcnet_tables.c",
        "opus/dnn/nnet.c",
        "opus/dnn/nnet_default.c",
        "opus/dnn/parse_lpcnet_weights.c",
        "opus/dnn/pitchdnn.c",
        "opus/dnn/pitchdnn_data.c",
        "opus/dnn/plc_data.c",
        # OSCE (LACE / NoLACE) — the addition over the private repo's list.
        # File list is xiph/opus's own OSCE_SOURCES (lpcnet_sources.mk) minus
        # dnn/bbwenet_data.c (OSCE-BWE; does not exist at this Opus
        # revision — see PINS.md and P4a's own verification note).
        "opus/dnn/osce.c",
        "opus/dnn/osce_features.c",
        "opus/dnn/nndsp.c",
        "opus/dnn/lace_data.c",
        "opus/dnn/nolace_data.c",
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        ".",
        "opus",
        "opus/include",
        "opus/celt",
        "opus/silk",
        "opus/silk/float",
        "opus/src",
        "opus/dnn"
      ],
      "defines": [
        "HAVE_CONFIG_H",
        "OPUS_BUILD",
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "cflags_c": [ "-std=c99", "-O3", "-fvisibility=hidden" ],
      "cflags_cc": [ "-std=c++17", "-O3", "-fvisibility=hidden" ],
      "xcode_settings": {
        "GCC_C_LANGUAGE_STANDARD": "c99",
        "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
        "MACOSX_DEPLOYMENT_TARGET": "10.15",
        "GCC_SYMBOLS_PRIVATE_EXTERN": "YES"
      },
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 0,
          "Optimization": 2,
          "PreprocessorDefinitions": [ "_CRT_SECURE_NO_WARNINGS", "_USE_MATH_DEFINES" ]
        }
      }
    }
  ]
}
