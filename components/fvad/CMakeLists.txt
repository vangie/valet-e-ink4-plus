# libfvad：WebRTC 人声检测（VAD）的独立版本，来自 https://github.com/dpirch/libfvad
# （提交 532ab666），BSD 许可，见 LICENSE / PATENTS。通话时用来判断麦克风里是不是有人在说话。
idf_component_register(
    SRCS
        "src/fvad.c"
        "src/signal_processing/division_operations.c"
        "src/signal_processing/energy.c"
        "src/signal_processing/get_scaling_square.c"
        "src/signal_processing/resample_48khz.c"
        "src/signal_processing/resample_by_2_internal.c"
        "src/signal_processing/resample_fractional.c"
        "src/signal_processing/spl_inl.c"
        "src/vad/vad_core.c"
        "src/vad/vad_filterbank.c"
        "src/vad/vad_gmm.c"
        "src/vad/vad_sp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function -Wno-unused-variable)
