# Keeps the 2021 metadata fetchable today, without touching any recipe:
#  - GitHub stopped serving the unauthenticated git:// protocol (2022), so
#    git://github.com/... sources are fetched over https;
#  - several projects renamed their default branch master -> main, so the
#    pinned SRCREV can't be found "in branch master" any more.
# Only the transport and the branch name change, SRCREVs stay the same.
PREMIRRORS_prepend = "\
    git://github.com/fmtlib/fmt.* git://github.com/fmtlib/fmt;protocol=https;branch=main \n \
    git://github.com/htop-dev/htop.* git://github.com/htop-dev/htop.git;protocol=https;branch=main \n \
    git://github.com/google/googletest.* git://github.com/google/googletest.git;protocol=https;branch=main \n \
    git://github.com/c-ares/c-ares.* git://github.com/c-ares/c-ares.git;protocol=https;branch=main \n \
    git://github.com/rsyslog/libfastjson.* git://github.com/rsyslog/libfastjson.git;protocol=https;branch=main \n \
    git://github.com/.* git://github.com/PATH;protocol=https \n \
    gitsm://github.com/.* gitsm://github.com/PATH;protocol=https \n \
"
