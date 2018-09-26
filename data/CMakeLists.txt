#

install(FILES molecube.service DESTINATION "${SYSTEMD_SERVICE_DIR}" RENAME molecube2.service)
install(FILES molecube.sysusers DESTINATION "${SYSTEMD_SYSUSERS_DIR}" RENAME molecube2.conf)
install(FILES molecube.tmpfiles DESTINATION "${SYSTEMD_TMPFILES_DIR}" RENAME molecube2.conf)
# Hard coded for now...
install(FILES molecube.yml DESTINATION "/etc")
