#

install(FILES molecube.service DESTINATION "${SYSTEMD_SERVICE_DIR}")
install(FILES molecube.sysusers DESTINATION "${SYSTEMD_SYSUSERS_DIR}" RENAME molecube.conf)
install(FILES molecube.tmpfiles DESTINATION "${SYSTEMD_TMPFILES_DIR}" RENAME molecube.conf)
# Hard coded for now...
install(FILES molecube.yml DESTINATION "/etc")
