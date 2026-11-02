# reimu-ota: over the OpenSSL the image already has, without GnuTLS
PACKAGECONFIG_remove_class-target = "gnutls libidn"
PACKAGECONFIG_append_class-target = " ssl"
