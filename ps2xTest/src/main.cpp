#include "MiniTest.h"
#include <cstdlib>
#include <iostream>

void register_code_generator_tests();
void register_r5900_decoder_tests();
void register_elf_analyzer_tests();
void register_pad_input_tests();
void register_ps2_runtime_io_tests();
void register_ps2_runtime_kernel_tests();
void register_ps2_runtime_interrupt_tests();
void register_ps2_memory_tests();
void register_ps2_vu1_tests();
void register_ps2_vu_tests();
void register_ps2_gs_tests();
void register_ps2_iop_tests();
void register_ps2_sif_rpc_tests();
void register_ps2_sif_dma_tests();
void register_ps2_recompiler_tests();
void register_ps2_runtime_expansion_tests();
void register_ps2_float_semantics_tests();
void register_gs_recording_tests();
void register_gs_gpu_tests();
void register_gs_tex_pack_tests();
void register_gs_font_tests();
void register_ui_settings_tests();
#if defined(PS2X_HAS_MODS_TESTS)
void register_mod_manager_tests();
#endif
void reset_ps2_test_function_table();

int main()
{
    // The GS tests read VRAM straight after drawing, so they need primitives drawn immediately
    // instead of queued for the drawer thread.
#ifdef _WIN32
    _putenv_s("PS2X_GS_DEFER", "0");
#else
    setenv("PS2X_GS_DEFER", "0", 1);
#endif

    MiniTest::BeforeEach(reset_ps2_test_function_table);

    register_code_generator_tests();
    register_r5900_decoder_tests();
    register_elf_analyzer_tests();
    register_pad_input_tests();
    register_ps2_runtime_io_tests();
    register_ps2_runtime_kernel_tests();
    register_ps2_runtime_interrupt_tests();
    register_ps2_memory_tests();
    register_ps2_vu1_tests();
    register_ps2_vu_tests();
    register_ps2_gs_tests();
    register_ps2_iop_tests();
    register_ps2_sif_rpc_tests();
    register_ps2_sif_dma_tests();
    register_ps2_recompiler_tests();
    register_ps2_runtime_expansion_tests();
    register_ps2_float_semantics_tests();
    register_gs_recording_tests();
    register_gs_gpu_tests();
    register_gs_tex_pack_tests();
    register_gs_font_tests();
    register_ui_settings_tests();
#if defined(PS2X_HAS_MODS_TESTS)
    register_mod_manager_tests();
#endif
    int res = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
