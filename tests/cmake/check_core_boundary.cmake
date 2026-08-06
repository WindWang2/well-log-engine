if(NOT DEFINED CORE_INCLUDE_DIR)
    message(FATAL_ERROR "CORE_INCLUDE_DIR was not provided")
endif()

file(GLOB_RECURSE core_headers "${CORE_INCLUDE_DIR}/*.hpp")
foreach(header IN LISTS core_headers)
    file(READ "${header}" contents)
    foreach(forbidden IN ITEMS
            "Qt"
            "Python"
            "pybind"
            "OpenGL"
            "GL/"
            "arrow/")
        string(FIND "${contents}" "${forbidden}" position)
        if(NOT position EQUAL -1)
            message(
                FATAL_ERROR
                "Core public header ${header} contains forbidden dependency token ${forbidden}"
            )
        endif()
    endforeach()
endforeach()
