# Install rules + CPack (.deb only; Linux amd64).
include(GNUInstallDirs)

install(TARGETS zterminal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# ssh's SSH_ASKPASS helper for vault-stored passwords (not for direct use).
install(TARGETS zterminal-askpass RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/zterminal)
install(PROGRAMS ${CMAKE_SOURCE_DIR}/packaging/zt DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/packaging/zterminal.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/zterminal RENAME copyright)

set(CPACK_PACKAGE_NAME "zterminal")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Stephen B. Johnson")
set(CPACK_PACKAGE_CONTACT "Stephen B. Johnson <49662809+sbj-ee@users.noreply.github.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/sbj-ee/zterminal")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PuTTY-like terminal emulator for Linux (Qt 6, libvterm)")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
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
include(CPack)
