# 6.3 dropped .probe_new: .probe takes the one argument now
do_configure_append() {
    sed -i 's/\.probe_new\b/.probe/' ${S}/l-pcs-i2c.c
}
