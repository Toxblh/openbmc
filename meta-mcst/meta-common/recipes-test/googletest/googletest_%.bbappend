# The project renamed its default branch master -> main (the pinned SRCREV is
# unchanged); GitHub serves git only over https. See reimu-fetch-fixes.inc.
SRC_URI := "${@d.getVar('SRC_URI', False).replace('git://github.com/google/googletest.git', 'git://github.com/google/googletest.git;protocol=https;branch=main')}"
