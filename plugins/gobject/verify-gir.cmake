if(NOT DEFINED WW_GIR_FILE)
    message(FATAL_ERROR "WW_GIR_FILE is required")
endif()

file(READ "${WW_GIR_FILE}" _ww_gir)
string(FIND "${_ww_gir}" "<member name=\"pause_blur\"" _ww_pause_blur)
if(_ww_pause_blur EQUAL -1)
    message(FATAL_ERROR
        "PresentationCapability.PAUSE_BLUR is missing from ${WW_GIR_FILE}")
endif()
