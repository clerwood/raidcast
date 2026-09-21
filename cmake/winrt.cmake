# C++/WinRT projection headers ship inside the Windows SDK, but MSVC does not
# put them on the include path for plain CMake targets the way the VS C++/WinRT
# project system does. Locate the newest SDK that has them.

function(raidcast_enable_cppwinrt target)
    if(NOT RAIDCAST_CPPWINRT_DIR)
        set(_roots
            "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Include"
            "C:/Program Files (x86)/Windows Kits/10/Include"
            "C:/Program Files/Windows Kits/10/Include")
        foreach(_root ${_roots})
            file(GLOB _vers "${_root}/10.*")
            list(SORT _vers)
            list(REVERSE _vers)
            foreach(_v ${_vers})
                if(EXISTS "${_v}/cppwinrt/winrt/base.h")
                    set(RAIDCAST_CPPWINRT_DIR "${_v}/cppwinrt" CACHE PATH "C++/WinRT headers" FORCE)
                    break()
                endif()
            endforeach()
            if(RAIDCAST_CPPWINRT_DIR)
                break()
            endif()
        endforeach()
    endif()

    if(NOT RAIDCAST_CPPWINRT_DIR)
        message(FATAL_ERROR
            "C++/WinRT headers not found. Install a Windows 10/11 SDK with the "
            "C++/WinRT component, or set -DRAIDCAST_CPPWINRT_DIR=<sdk>/cppwinrt.")
    endif()

    target_include_directories(${target} PRIVATE "${RAIDCAST_CPPWINRT_DIR}")
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX          # windows.h min/max macros break std::min / std::max
        UNICODE
        _UNICODE
    )
endfunction()
