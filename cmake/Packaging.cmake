# Install rules + CPack: .deb on Linux amd64, .dmg (DragNDrop) on Apple Silicon.
# Version comes solely from project(zterminal VERSION ...) → PROJECT_VERSION.
# Artifact names:
#   zterminal_<ver>_amd64.deb          (Linux; in-app updater expects this)
#   zterminal-<ver>-Darwin.dmg         (macOS arm64; zterminal.app + /Applications)

include(GNUInstallDirs)

set(CPACK_PACKAGE_NAME "zterminal")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Stephen B. Johnson")
set(CPACK_PACKAGE_CONTACT "Stephen B. Johnson <49662809+sbj-ee@users.noreply.github.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/sbj-ee/zterminal")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "PuTTY-like terminal emulator for Linux amd64 and Apple Silicon (Qt 6, libvterm)")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

if(UNIX AND NOT APPLE)
  install(TARGETS zterminal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  # ssh's SSH_ASKPASS helper for vault-stored passwords (not for direct use).
  install(TARGETS zterminal-askpass RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/zterminal)
  install(PROGRAMS ${CMAKE_SOURCE_DIR}/packaging/zt DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/zterminal.desktop
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
          DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/zterminal RENAME copyright)
  foreach(_sz 16 24 32 48 64 128 256 512)
    if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icons/zterminal-${_sz}.png")
      install(FILES ${CMAKE_SOURCE_DIR}/assets/icons/zterminal-${_sz}.png
              DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_sz}x${_sz}/apps
              RENAME zterminal.png)
    endif()
  endforeach()

  set(CPACK_GENERATOR "DEB")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
  set(CPACK_DEBIAN_PACKAGE_SECTION "x11")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  # Qt's Wayland platform plugin is what gives PRIMARY selection on GNOME Wayland;
  # libqt6serialport6 is the serial backend and libsodium23 the vault crypto
  # (shlibdeps finds both too; listed explicitly).
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-wayland, libqt6serialport6, libsodium23")
  # Help > Check for Updates installs through `pkexec apt install` (falls back
  # to the release page without it). shlibdeps adds libqt6network6 itself.
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pkexec | policykit-1")
  # zterminal_<ver>_amd64.deb (the name the updater expects).
  set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
elseif(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_DMG_VOLUME_NAME "zterminal ${PROJECT_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME "zterminal-${PROJECT_VERSION}-Darwin")
  # No license-agreement prompt when the dmg is mounted (CMake >= 3.23); the
  # licence is in the repository and the About box.
  set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF)
  # zterminal.app at the dmg root next to CPack's /Applications symlink.
  # MacDeploy.cmake then runs macdeployqt, strips Homebrew rpaths, ad-hoc
  # signs (nested then app; no hardened runtime until Developer ID) and check-bundle.sh.
  install(TARGETS zterminal BUNDLE DESTINATION .)
  install(TARGETS zterminal-askpass
          RUNTIME DESTINATION zterminal.app/Contents/MacOS)
  # Generated icns: ensure it lands in the staged .app (BUNDLE install can miss
  # a generated resource living outside the app/ binary dir).
  if(DEFINED ZTERMINAL_ICNS_FILE)
    install(FILES "${ZTERMINAL_ICNS_FILE}"
            DESTINATION zterminal.app/Contents/Resources)
  endif()
  install(SCRIPT "${CMAKE_BINARY_DIR}/MacDeploy.cmake")
else()
  message(FATAL_ERROR "zterminal: packaging is only configured for Linux amd64 and macOS arm64")
endif()

include(CPack)
message(STATUS "zterminal: CPack configured (version ${PROJECT_VERSION})")
