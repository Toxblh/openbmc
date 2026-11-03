# VNC (5900) on localhost only, as upstream has it: the web UI reaches it
# through bmcweb /kvm/0 (443); a VNC client through ssh -L 5900:localhost:5900.
# Open to the LAN it offered security type None (no password) to anyone.
SRC_URI_remove = "file://0000-allow-connect-from-all-ip-addresses.patch"
