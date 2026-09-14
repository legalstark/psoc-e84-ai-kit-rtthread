    .syntax unified

    .section .cy_socmem_data,"a",%progbits
    .balign 16
    .global ai_kit_npu_person_model_start
    .global ai_kit_npu_person_model_end
ai_kit_npu_person_model_start:
    .incbin "applications/ai_kit_npu/TFLM_PERSON_U55_int8x8.tflite"
ai_kit_npu_person_model_end:

    .section .rodata.ai_kit_npu_sample,"a",%progbits
    .balign 16
    .global ai_kit_npu_person_sample_start
    .global ai_kit_npu_person_sample_end
ai_kit_npu_person_sample_start:
    .incbin "applications/ai_kit_npu/TFLM_PERSON_U55_samples2.bin", 16, 9216
ai_kit_npu_person_sample_end:

    .balign 16
    .global ai_kit_npu_no_person_sample_start
    .global ai_kit_npu_no_person_sample_end
ai_kit_npu_no_person_sample_start:
    .incbin "applications/ai_kit_npu/TFLM_PERSON_U55_samples2.bin", 9232, 9216
ai_kit_npu_no_person_sample_end:

    .section .note.GNU-stack,"",%progbits
