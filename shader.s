                section .rodata

                global g_spv_vs
                global g_spv_vs_size
                global g_spv_fs
                global g_spv_fs_size

                align 16
g_spv_vs:       incbin "vs.spv"
g_spv_vs_size:  dd $ - g_spv_vs
                align 16
g_spv_fs:       incbin "fs.spv"
g_spv_fs_size:  dd $ - g_spv_fs
