# the VNC password check (single DES) with libvncserver's own code: in
# OpenSSL 3 single DES is only in the legacy provider, not loaded, and every
# VNC password was refused. Nothing else of OpenSSL is used (no VNC TLS, no
# websockets: bmcweb proxies /kvm/0 to obmc-ikvm on localhost)
PACKAGECONFIG_remove = "openssl"
