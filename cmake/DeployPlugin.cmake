if(NOT DEFINED DEPLOY_SOURCE OR NOT EXISTS "${DEPLOY_SOURCE}")
    message(FATAL_ERROR "Deploy source does not exist: ${DEPLOY_SOURCE}")
endif()
if(NOT DEFINED DEPLOY_DIRECTORY OR DEPLOY_DIRECTORY STREQUAL "" OR
   NOT DEFINED DEPLOY_FILE_NAME OR DEPLOY_FILE_NAME STREQUAL "")
    message(FATAL_ERROR "Runtime deploy destination is not configured")
endif()

file(MAKE_DIRECTORY "${DEPLOY_DIRECTORY}")
set(destination "${DEPLOY_DIRECTORY}/${DEPLOY_FILE_NAME}")
message(STATUS "Deploy source: ${DEPLOY_SOURCE}")
message(STATUS "Deploy destination: ${destination}")

# COPY_FILE without ONLY_IF_DIFFERENT always performs an overwrite. A loaded
# .auf2 is locked by AviUtl2, so report that as a build failure instead of
# leaving an old runtime binary behind while claiming success.
file(COPY_FILE "${DEPLOY_SOURCE}" "${destination}" RESULT copy_result)
if(NOT copy_result STREQUAL "0")
    message(FATAL_ERROR
        "Runtime deploy failed: ${copy_result}\n"
        "Please exit AviUtl2, then run the same build command again.\n"
        "AviUtl2を終了してから、同じビルドコマンドを再実行してください。")
endif()

if(NOT EXISTS "${destination}")
    message(FATAL_ERROR "Deploy destination was not created: ${destination}")
endif()
file(SIZE "${DEPLOY_SOURCE}" source_size)
file(SIZE "${destination}" destination_size)
file(SHA256 "${DEPLOY_SOURCE}" source_sha256)
file(SHA256 "${destination}" destination_sha256)
if(NOT source_size EQUAL destination_size OR
   NOT source_sha256 STREQUAL destination_sha256)
    message(FATAL_ERROR
        "Runtime deploy verification failed: size or SHA-256 mismatch")
endif()
message(STATUS "Deploy completed")
message(STATUS "Deploy SHA-256: ${destination_sha256}")
