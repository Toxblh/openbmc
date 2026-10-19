# Keeps the 2021 metadata fetchable today:
#  - GitHub stopped serving the unauthenticated git:// protocol (2022), so
#    git://github.com/... sources are fetched over https (PREMIRRORS, no
#    recipe changes);
#  - fmt, htop, googletest, c-ares and libfastjson renamed their default
#    branch master -> main. A mirror can't fix that (do_unpack checks the
#    SRCREV against the branch of the original URL), so these five have
#    bbappends in meta-common that set branch=main. SRCREVs are unchanged.
PREMIRRORS_prepend = "\
    git://github.com/.* git://github.com/PATH;protocol=https \n \
    gitsm://github.com/.* gitsm://github.com/PATH;protocol=https \n \
"
