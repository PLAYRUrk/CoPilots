# ── PDFium (prebuilt, Windows x64) ─────────────────────────────────────────
# The plugin never links pdfium: ChartRenderer LoadLibrary()s pdfium.dll from
# the plugin folder at runtime and self-declares the tiny C ABI it needs, so
# no headers or import libraries are required — only the DLL itself, staged
# next to win.xpl.  If the download fails (offline build), the plugin still
# builds and runs; PDF charts are disabled until pdfium.dll is dropped in.
#
# Override with -DPDFIUM_DLL=<path to pdfium.dll> to skip the download.

if(WIN32)
    if(NOT DEFINED PDFIUM_DLL)
        set(PDFIUM_VERSION "chromium/7047")
        set(PDFIUM_URL
            "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F7047/pdfium-win-x64.tgz")
        set(PDFIUM_ARCHIVE "${CMAKE_BINARY_DIR}/pdfium/pdfium-win-x64.tgz")
        set(PDFIUM_EXTRACT_DIR "${CMAKE_BINARY_DIR}/pdfium/extracted")
        set(PDFIUM_DLL "${PDFIUM_EXTRACT_DIR}/bin/pdfium.dll")

        if(NOT EXISTS "${PDFIUM_DLL}")
            message(STATUS "Downloading PDFium ${PDFIUM_VERSION}...")
            file(DOWNLOAD "${PDFIUM_URL}" "${PDFIUM_ARCHIVE}"
                 STATUS PDFIUM_DL_STATUS SHOW_PROGRESS)
            list(GET PDFIUM_DL_STATUS 0 PDFIUM_DL_CODE)
            if(PDFIUM_DL_CODE EQUAL 0)
                file(MAKE_DIRECTORY "${PDFIUM_EXTRACT_DIR}")
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E tar xzf "${PDFIUM_ARCHIVE}"
                    WORKING_DIRECTORY "${PDFIUM_EXTRACT_DIR}"
                    RESULT_VARIABLE PDFIUM_EXTRACT_CODE)
                if(NOT PDFIUM_EXTRACT_CODE EQUAL 0)
                    message(WARNING "PDFium archive extraction failed — "
                                    "PDF charts will be disabled at runtime.")
                endif()
            else()
                message(WARNING "PDFium download failed (${PDFIUM_DL_STATUS}) — "
                                "PDF charts will be disabled at runtime.")
            endif()
        endif()
    endif()

    if(EXISTS "${PDFIUM_DLL}")
        message(STATUS "PDFium DLL: ${PDFIUM_DLL}")
    else()
        set(PDFIUM_DLL "")
    endif()
endif()
