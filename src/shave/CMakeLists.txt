# Copyright © 2024 Intel Corporation
# SPDX-License-Identifier: Apache 2.0
# LEGAL NOTICE: Your use of this software and any required dependent software (the “Software Package”)
# is subject to the terms and conditions of the software license agreements for the Software Package,
# which may also include notices, disclaimers, or license terms for third party or open source software
# included in or with the Software Package, and your use indicates your acceptance of all such terms.
# Please refer to the “third-party-programs.txt” or other similarly-named text file included with the
# Software Package for additional details.

function(vpu_cost_model_safe_file_write file content)
    # tracking of rewrite is required to avoid rebuild of the whole project
    # in case of cmake rerun. Need to rebuild only if content of content is changed
    set(rewrite_file ON)
    if(EXISTS ${file})
        file(READ ${file} current_content)
        string(SHA256 current_hash "${current_content}")
        string(SHA256 new_hash "${content}")
        if(current_hash STREQUAL new_hash)
            set(rewrite_file OFF)
        endif()
    endif()

    if(rewrite_file)
        file(WRITE ${file} "${content}")
    endif()
endfunction()

set (LIST_LEGACY_CSV "")

# clasic shave  (old/Deprecated) based on latency and efficiency (linear)
function(GENERATE_SHV_KERNEL KERNEL_TYPE KERNEL_NAME)
    set(K_NAME_csv "${KERNEL_NAME}")# original from csv
    set(K_TYPE_csv "${KERNEL_TYPE}")# original from csv

    set(KERNEL_NAME "SHV${KERNEL_NAME}")
    set(KERNEL_TYPE "SHV_${KERNEL_TYPE}_CLASS")

    if(ARGC EQUAL 2)
        set(KERNEL_EFFICIENCY 1.0)
        set(KERNEL_LATENCY 0)
    elseif(ARGC EQUAL 4)
        set(KERNEL_EFFICIENCY ${ARGV2})
        set(KERNEL_LATENCY ${ARGV3})
    else()
        message(FATAL_ERROR "Wrong number of parameters in GENERATE_SHV_KERNEL function")
    endif()

    # message(STATUS "Generate SHV Kernel ${KERNEL_NAME} with efficiency ${KERNEL_EFFICIENCY} bytes/cycle and latency ${KERNEL_LATENCY} cycles")
    set(GEN_HEADER_FILE "vpu/shave/${KERNEL_NAME}.h")
    set(GEN_HEADER_FULL_FILE "${CMAKE_VPUNN_BINARY_DIR}/include/${GEN_HEADER_FILE}")

    set(content "
#pragma once

#include \"vpu/shave/activation.h\"
#include \"vpu/shave/data_movement.h\"
#include \"vpu/shave/elementwise.h\"

namespace VPUNN \{

struct ${KERNEL_NAME} : public ${KERNEL_TYPE}<int(${KERNEL_EFFICIENCY} * 1000), ${KERNEL_LATENCY}> \{
    using ${KERNEL_TYPE}::${KERNEL_TYPE}\;
\}\;

\}

")
    vpu_cost_model_safe_file_write("${GEN_HEADER_FULL_FILE}" ${content})

    set(ADD_LINE "Add<${KERNEL_NAME}, int(${KERNEL_EFFICIENCY}F * 1000), ${KERNEL_LATENCY}>(\"${K_NAME_csv}\")\;  //${K_TYPE_csv}")
    #message(STATUS " >                  ${ADD_LINE}")

    set(LIST_LEGACY_CSV "${LIST_LEGACY_CSV}${ADD_LINE} \n " PARENT_SCOPE)

    set(GLOBAL_INCLUDE_FILE "${GLOBAL_INCLUDE_FILE}#include \"${GEN_HEADER_FILE}\"\n" PARENT_SCOPE)
endfunction()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/layers_perf.csv" config)
STRING(REPLACE "null" "" config "${config}")
STRING(REPLACE " " "" config "${config}")
STRING(REPLACE "\n" ";" config "${config}")

set(SKIP_HEADER TRUE)

foreach(LINE IN LISTS config)
    if(SKIP_HEADER)
        set(SKIP_HEADER FALSE)
    else()
        STRING(REPLACE "," ";" PARAMETERS ${LINE})
        GENERATE_SHV_KERNEL(${PARAMETERS})
    endif()
endforeach()

vpu_cost_model_safe_file_write("${CMAKE_VPUNN_BINARY_DIR}/include/vpu/shave/layers.h" ${GLOBAL_INCLUDE_FILE})

#place inl file for legacy
#message(STATUS " >> Generated code for Shave Legacy is : \n ${LIST_LEGACY_CSV} \n  >> Generated code END")
set (SHAVE_ADD_LIST "${LIST_LEGACY_CSV}")

message(STATUS " >> Generated Collection initializer for Shave Legacy is : \n ${SHAVE_ADD_LIST} \n  >> Generated code END")

vpu_cost_model_safe_file_write("${CMAKE_VPUNN_BINARY_DIR}/include/vpu/shave/SHAVE_V27_Linear.inl" ${SHAVE_ADD_LIST})

#NEW SHAVE modeling.  v27 CSV import

function(GENERATE_SHV_INITIALIZER kernel_type kernel_name slope intercept scalar_offset unroll_offset unroll_size vector_size DPU_frequency SHV_frequency)
    if(ARGC EQUAL 10)
        # use the passed params
    else()
        message(FATAL_ERROR "Wrong number of parameters in GENERATE_SHV_INITIALIZER function: ${ARGC} instead of  10 , \n ${ARGV}")
    endif()

    set(data_type "DataType::FLOAT16")

    if(kernel_type STREQUAL "ShaveModel1on1" OR kernel_type STREQUAL "ShaveModel2on1")
        set (ADD_LINE "Add<${data_type},${vector_size},${unroll_size},${DPU_frequency},${SHV_frequency} >(\"${kernel_name}\", (float)${slope} , (float)${intercept}, (float)${scalar_offset}, (float)${unroll_offset})\;  //${kernel_type}")
        message(STATUS "Parse SHV Kernel ${kernel_name} with: ${kernel_type} => ${slope},${intercept},${scalar_offset},${unroll_offset},${unroll_size},${vector_size},${DPU_frequency},${SHV_frequency}")
        # message(STATUS " >                  ${ADD_LINE}")
    endif()

    set (LIST_INITIALIZER_FROM_CSV "${LIST_INITIALIZER_FROM_CSV}${ADD_LINE} \n " PARENT_SCOPE)
endfunction()

function(GENERATE_SHV_INITIALIZER_NPU40 kernel_type kernel_name slope intercept unroll_offset intra_block_offset vector_offset displacement_size unroll_size vector_size DPU_frequency SHV_frequency)
    if(ARGC EQUAL 12)
        # use the passed params
    else()
        message(FATAL_ERROR "Wrong number of parameters in GENERATE_SHV_INITIALIZER function: ${ARGC} instead of  11 , \n ${ARGV}")
    endif()

    set(data_type "DataType::FLOAT16")
    # message(STATUS "Parse SHV Kernel ${kernel_name} with: ${kernel_type} => ${slope},${intercept},${scalar_offset},${unroll_offset},${unroll_size},${vector_size},${DPU_frequency},${SHV_frequency}")


    if(kernel_type STREQUAL "ShaveModel1on1NPU40" OR kernel_type STREQUAL "ShaveModel2on1NPU40")
        message(STATUS "Parse SHV Kernel ${kernel_name} with: ${kernel_type} => ${slope},${intercept},${unroll_offset},${intra_block_offset},${vector_offset},${unroll_size},${vector_size},${DPU_frequency},${SHV_frequency}")
        set (ADD_LINE "Add<${data_type},${vector_size},${unroll_size},${DPU_frequency},${SHV_frequency} >(\"${kernel_name}\", (float)${slope} , (float)${intercept}, (float)${unroll_offset}, (float)${intra_block_offset}, (float)${vector_offset}, ${displacement_size})\;  //${kernel_type}")
    endif()

    set (LIST_INITIALIZER_FROM_CSV "${LIST_INITIALIZER_FROM_CSV}${ADD_LINE} \n " PARENT_SCOPE)
endfunction()

function(GET_MODEL_TYPE kernel_type)
    # message(STATUS "Identify SHV Kernel ${kernel_type}")
    if(kernel_type STREQUAL "ShaveModel1on1" OR kernel_type STREQUAL "ShaveModel2on1")
        set(kmode "KType1") # this is the type for 1-1 or 2-1 simple
        # message(STATUS "Identified as  mode1: ${kmode}")
    elseif(kernel_type STREQUAL "ShaveModel1on1NPU40" OR kernel_type STREQUAL "ShaveModel2on1NPU40")
        set(kmode "KType1_NPU40") # this is the type for 1-1 or 2-1 simple for NPU 40
    else()
        set(kmode "KTypeUnknown")
        # message(STATUS "Identified as  not recognized: ${kmode}")
    endif()

    set(KERNEL_INFO ${kmode} PARENT_SCOPE)
    # message(STATUS "Identified as: ${KERNEL_INFO}, ${kmode}")
endfunction()


##   # Generating for 2.7
##   message(STATUS "\n >> Starting to generate initializer for Shave 27 ")
##   file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/shave_layers_vpu_2_7.csv" config)
##   STRING(REPLACE "null" "" config "${config}")
##   STRING(REPLACE " " "" config "${config}")
##   STRING(REPLACE "\n" ";" config "${config}")
##   
##   set(SKIP_HEADER TRUE)
##   set(LIST_INITIALIZER_FROM_CSV "")
##   
##   foreach(LINE IN LISTS config)
##       if(SKIP_HEADER)
##           set(SKIP_HEADER FALSE)
##       else()
##           STRING(REPLACE "," ";" PARAMETERS ${LINE})
##   
##           set (KERNEL_INFO "XXX")
##           GET_MODEL_TYPE(${PARAMETERS})
##   
##           if(KERNEL_INFO STREQUAL "KType1" )
##               GENERATE_SHV_INITIALIZER(${PARAMETERS})
##           elseif(KERNEL_INFO STREQUAL "KTypeUnknown"  )
##            message(STATUS " >    Line to parse not known:              ${PARAMETERS}")
##           else()
##            message(STATUS " >   ERROR in  Line to parse (unexpected kernel info):     ${KERNEL_INFO}  ::params=        ${PARAMETERS}")
##           endif()
##       endif()
##   endforeach()
##   
##   #place inl file for legacy
##   set(SHAVE_ADD_LIST "${LIST_INITIALIZER_FROM_CSV}")
##   message(STATUS " >> Generated Collection initializer for Shave 27 is : \n ${SHAVE_ADD_LIST} \n  >> Generated code END")
##   vpu_cost_model_safe_file_write("${CMAKE_VPUNN_BINARY_DIR}/include/vpu/shave/SHAVE_V27.inl" ${SHAVE_ADD_LIST})  #file is implicitly without prev content
##   #end of generation of NPU2.7  collection from CSV


function(PARSE_CSV csv_file  inl_out_file  info)
    message(STATUS "\n >> Starting to generate initializer for ${info}")
    message(STATUS "\n >> PARAMS:\n csv_file: " ${csv_file} "\n inl_out_file: " ${inl_out_file} "\n info: " ${info} )

    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${csv_file}" config)
  # message(STATUS "\n >> config: " ${config})  # content of file
    STRING(REPLACE "null" "" config "${config}")
    STRING(REPLACE " " "" config "${config}")
    STRING(REPLACE "\n" ";" config "${config}")

    set(SKIP_HEADER TRUE)
    set(LIST_INITIALIZER_FROM_CSV "")

    foreach(LINE IN LISTS config)
        if(SKIP_HEADER)
            set(SKIP_HEADER FALSE)
        else()
            STRING(REPLACE "," ";" PARAMETERS ${LINE})

            set (KERNEL_INFO "XXX")
            GET_MODEL_TYPE(${PARAMETERS})

            if(KERNEL_INFO STREQUAL "KType1")
             GENERATE_SHV_INITIALIZER(${PARAMETERS})
            elseif(KERNEL_INFO STREQUAL "KType1_NPU40")
             GENERATE_SHV_INITIALIZER_NPU40(${PARAMETERS})
            elseif(KERNEL_INFO STREQUAL "KTypeUnknown"  )
             message(STATUS " >    Line to parse not known:              ${PARAMETERS}")
            else()
             message(STATUS " >   ERROR in  Line to parse (unexpected kernel info):     ${KERNEL_INFO}  ::params=        ${PARAMETERS}")
            endif()
        endif()
    endforeach()

    #place inl file for legacy
    set(SHAVE_ADD_LIST "${LIST_INITIALIZER_FROM_CSV}")
    message(STATUS " >> Generated Collection initializer for ${info} is : \n ${SHAVE_ADD_LIST} \n  >> Generated code END (${info}) \n")
    vpu_cost_model_safe_file_write("${CMAKE_VPUNN_BINARY_DIR}/include/vpu/shave/${inl_out_file}" ${SHAVE_ADD_LIST})  #file is implicitly without prev content
endfunction()


set(P1 "shave_layers_vpu_2_7.csv")
set(P2 "SHAVE_V27.inl")
#PARSE_CSV( shave_layers_vpu_2_7.csv SHAVE_V27.inl)

PARSE_CSV( ${P1} ${P2} "Shave NPU27 WIP")

# Deactivating till figuring out the output format

set(P1 "shave_layers_vpu_40.csv") #must contain at least one input
set(P2 "SHAVE_V40.inl")
PARSE_CSV( ${P1} ${P2} "Shave NPU40 WIP")

# ####################################################################################################

# shave library for new cpps
include_directories(${CMAKE_CURRENT_BINARY_DIR}/../../include)
include_directories(${FLATBUFFERS_SRC_DIR}/include)

add_library(vpunn_shave shave_collection.cpp shave_cache.cpp)
# add_dependencies(vpunn_shave_shared cpp_schema)
# target_link_libraries(vpunn_shave_shared flatbuffers)

if(VPUNN_BUILD_SHARED_LIB)
    add_library(vpunn_shave_shared SHARED shave_collection.cpp)
    # add_dependencies(vpunn_shave_shared cpp_schema)
    # target_link_libraries(vpunn_shave_shared flatbuffers)

    set_target_properties(vpunn_shave_shared PROPERTIES OUTPUT_NAME vpunn_shave)
endif()
