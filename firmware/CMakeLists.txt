cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(beecord-firmware)

# micro-opus 组件（M3）：libopus SILK 在 GCC 12+ -O2 下触发已知的
# maybe-uninitialized 误报（libopus 上游同样存在，NLSF2A cos_LSF_QA），
# IDF 默认 -Werror 会据此失败；仅对该组件把此警告降级为不报错，
# 其余组件与本项目代码保持 -Werror 零警告门禁。
idf_component_get_property(micro_opus_lib esphome__micro-opus COMPONENT_LIB)
target_compile_options(${micro_opus_lib} PRIVATE -Wno-error=maybe-uninitialized)
